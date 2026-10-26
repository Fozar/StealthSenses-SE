// Stealth Senses — SKSE plugin for Skyrim SE/AE
// Copyright (C) 2026 fozar
//
// This program is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

// Dev build only (CMakeLists adds this file with STEALTHSENSES_DEV)
#include "Overlay.h"

#include "Config.h"
#include "Profiles.h"
#include "Trackers.h"
#include "Trail.h"
#include "ScentLogic.h"

// RE headers first: <Windows.h> macros break CommonLib declarations
#include <d3d11.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace StealthSenses::Overlay {
    namespace {
        // Footprints, marks and trackers this far from the player are drawn
        constexpr float kRadius      = 4000.0f;
        // Footprints closer than this get a label, closer than kDetailRadius a second line
        constexpr float kLabelRadius  = 700.0f;
        constexpr float kDetailRadius = 350.0f;
        // Consecutive footprints closer than this are joined by a line (farther: a gap, thinned)
        constexpr float kLinkDistance = 400.0f;

        struct Dot {
            RE::NiPoint3    pos;
            float           vis   = 0.0f;
            float           step  = 1.0f;
            std::uint32_t   seq   = 0;
            RE::MATERIAL_ID material = RE::MATERIAL_ID::kNone;
            bool            blood = false;
            bool            link  = false;  // a line to the previous dot
            float           scent = 0.0f, age = 0.0f, scentRadius = 0.0f;
            float           spreadRadius = 0.0f, readerRadius = -1.0f;
            std::uint32_t   segment = 0;
            bool            scentLink = false, ring = false;
        };

        struct Pin {
            RE::NiPoint3 pos;
            int          number = 0;
        };

        struct Snapshot {
            RE::FormID                      space = 0;
            std::chrono::steady_clock::time_point sampledAt{};
            bool                            current = false;
            Config::ScentSettings            scent;
            std::size_t                      scented = 0, legacy = 0;
            std::vector<Dot>                 dots;
            std::vector<Pin>                 pins;
            std::vector<Trackers::DebugView> trackers;
            std::string                      step;
            std::vector<std::string>         reader;
            std::size_t                      here = 0, readable = 0, clear = 0;
            bool                             badWeather = false;
            float                            faint = 0.1f, clearVis = 0.2f, searchRadius = 700.0f;
        };

        struct Mark {
            RE::NiPoint3 pos;
            RE::FormID   space  = 0;
            int          number = 0;
        };

        std::atomic<unsigned> g_mode{ 0 }; // off, visual, scent, both
        std::mutex        g_lock;        // g_snapshot: task threads write, Present reads
        Snapshot          g_snapshot;
        std::vector<Mark> g_marks;       // task threads only

        // Present thread only
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
        PresentFn                g_present = nullptr;
        bool                     g_imgui   = false;
        bool                     g_failed  = false;
        ID3D11Device*            g_device  = nullptr;
        ID3D11DeviceContext*     g_context = nullptr;
        std::chrono::steady_clock::time_point g_lastFrame;

        // World point to screen pixels; false behind the camera
        struct Projector {
            float              matrix[4][4]{};
            RE::NiRect<float>  port;
            float              width = 0.0f, height = 0.0f;
            bool               ok = false;

            bool operator()(const RE::NiPoint3& a_pos, ImVec2& a_out) const {
                float x = 0.0f, y = 0.0f, z = 0.0f;
                // [CL] NiCamera::WorldPtToScreenPt3, x/y in 0..1 of the port, y up
                if (!ok || !RE::NiCamera::WorldPtToScreenPt3(matrix, port, a_pos, x, y, z, 1e-5f) || z < 0.0f) {
                    return false;
                }
                a_out = { x * width, (1.0f - y) * height };
                return true;
            }
        };

        ImU32 VisColor(float a_vis, const Snapshot& a_s) {
            if (a_vis >= a_s.clearVis) {
                return IM_COL32(60, 220, 90, 240);
            }
            if (a_vis >= a_s.faint) {
                return IM_COL32(240, 200, 40, 235);
            }
            return IM_COL32(150, 150, 150, 150);
        }

        void Text(ImDrawList* a_list, ImVec2 a_pos, ImU32 a_color, const std::string& a_text) {
            a_list->AddText({ a_pos.x + 1.0f, a_pos.y + 1.0f }, IM_COL32(0, 0, 0, 220), a_text.c_str());
            a_list->AddText(a_pos, a_color, a_text.c_str());
        }

        // A horizontal circle in the world (search radius around the last footprint)
        void WorldRing(ImDrawList* a_list, const Projector& a_project, const RE::NiPoint3& a_center, float a_radius, ImU32 a_color) {
            constexpr int kSegments = 36;
            ImVec2        prev{};
            bool          prevOk = false;
            for (int i = 0; i <= kSegments; ++i) {
                const float  angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / kSegments;
                RE::NiPoint3 p{ a_center.x + a_radius * std::cos(angle), a_center.y + a_radius * std::sin(angle), a_center.z + 10.0f };
                ImVec2       screen{};
                const bool   ok = a_project(p, screen);
                if (ok && prevOk) {
                    a_list->AddLine(prev, screen, a_color, 1.5f);
                }
                prev   = screen;
                prevOk = ok;
            }
        }

        void DrawScene(const Snapshot& a_s, const Projector& a_project, const RE::NiPoint3& a_player, unsigned a_mode) {
            auto* list = ImGui::GetBackgroundDrawList();
            const bool visual = a_s.current && (a_mode == 1 || a_mode == 3);
            const bool smell = a_s.current && (a_mode == 2 || a_mode == 3);

            // Trail: lines first, dots over them
            ImVec2 prev{};
            bool   prevOk = false;
            for (const auto& dot : a_s.dots) {
                ImVec2     screen{};
                const bool ok = a_project(dot.pos, screen);
                if (ok && prevOk && visual && dot.link) {
                    list->AddLine(prev, screen, IM_COL32(200, 200, 200, 90), 1.0f);
                }
                if (ok && prevOk && smell && dot.scentLink) {
                    list->AddLine(prev, screen, IM_COL32(20, 200, 200, 160), 2.0f);
                }
                prev   = screen;
                prevOk = ok;
            }
            for (const auto& dot : a_s.dots) {
                ImVec2 screen{};
                if (!a_project(dot.pos, screen)) {
                    continue;
                }
                const float dist   = dot.pos.GetDistance(a_player);
                const float radius = std::clamp(2500.0f / std::max(dist, 1.0f), 2.5f, 7.0f);
                if (visual) list->AddCircleFilled(screen, radius, VisColor(dot.vis, a_s));
                if (visual && dot.blood) {
                    list->AddCircle(screen, radius + 2.0f, IM_COL32(230, 30, 30, 255), 0, 2.0f);
                }
                if (visual && dist <= kLabelRadius) {
                    Text(list, { screen.x + radius + 3.0f, screen.y - 7.0f }, IM_COL32(255, 255, 255, 230),
                        std::format("#{} {:.2f}", dot.seq, dot.vis));
                    if (dist <= kDetailRadius) {
                        Text(list, { screen.x + radius + 3.0f, screen.y + 6.0f }, IM_COL32(200, 200, 200, 210),
                            std::format("{} x{:.2f}", Trail::MaterialName(dot.material), dot.step));
                    }
                }
                if (smell && dot.segment && dot.scent >= a_s.scent.min_strength) {
                    const auto alpha = static_cast<int>(std::clamp(dot.scent / std::max(a_s.scent.base_strength, 0.1f), 0.2f, 1.0f) * 240.0f);
                    const auto color = IM_COL32(30, 225, 210, alpha);
                    if (visual) list->AddCircle(screen, radius + 4.0f, color, 0, 2.0f);
                    else list->AddCircleFilled(screen, radius, color);
                    if (dot.blood) list->AddCircle(screen, radius + 2.0f, IM_COL32(230, 30, 30, 220), 0, 2.0f);
                    if (dot.ring) {
                        const float fade = std::clamp(dot.scent / std::max(a_s.scent.base_strength, 0.1f), 0.0f, 1.0f);
                        WorldRing(list, a_project, dot.pos, dot.spreadRadius, IM_COL32(30, 225, 210, static_cast<int>(80 * fade)));
                        const float sensed = dot.readerRadius >= 0 ? dot.readerRadius : dot.scentRadius;
                        if (sensed > 0) WorldRing(list, a_project, dot.pos, sensed, IM_COL32(100, 255, 230, static_cast<int>(190 * fade)));
                    }
                    if (dist <= kDetailRadius) {
                        Text(list, { screen.x + radius + 3.0f, screen.y + (visual ? 20.0f : -7.0f) }, color,
                            std::format("#{} S{:.2f} age {:.2f}h seg{} spread{:.0f} sense{:.0f}", dot.seq, dot.scent, dot.age, dot.segment,
                                dot.spreadRadius, dot.readerRadius >= 0 ? dot.readerRadius : dot.scentRadius));
                    }
                }
            }

            // Marks: a post with the number on top
            for (const auto& pin : a_s.pins) {
                ImVec2 foot{}, top{};
                auto   head = pin.pos;
                head.z += 150.0f;
                if (a_project(pin.pos, foot) && a_project(head, top)) {
                    list->AddLine(foot, top, IM_COL32(230, 80, 230, 255), 3.0f);
                    Text(list, { top.x - 8.0f, top.y - 16.0f }, IM_COL32(255, 120, 255, 255), std::format("M{}", pin.number));
                }
            }

            // Trackers: what it does over its head, a line to its marker, the search ring
            for (const auto& t : a_s.trackers) {
                ImVec2 feet{}, target{}, head{};
                auto   headPos = t.pos;
                headPos.z += 140.0f;
                const bool feetOk = a_project(t.pos, feet);
                if (feetOk && a_project(t.target, target)) {
                    list->AddLine(feet, target, IM_COL32(60, 200, 255, 220), 2.0f);
                    list->AddCircle(target, 6.0f, IM_COL32(60, 200, 255, 255), 0, 2.0f);
                }
                if (t.searching) {
                    WorldRing(list, a_project, t.trailPos, a_s.searchRadius, IM_COL32(255, 150, 40, 200));
                }
                if (a_project(headPos, head)) {
                    Text(list, { head.x - 40.0f, head.y - 14.0f }, IM_COL32(120, 220, 255, 255), t.label);
                }
            }

            // Panel in the corner
            ImGui::SetNextWindowPos({ 10.0f, 10.0f });
            ImGui::SetNextWindowBgAlpha(0.55f);
            ImGui::Begin("##StealthSensesOverlay", nullptr,
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
            ImGui::TextUnformatted("StealthSenses overlay (Num4)");
            ImGui::Text("Num4: %s -> next mode -> off", a_mode == 1 ? "VISUAL" : a_mode == 2 ? "SCENT" : "BOTH");
            if (!a_s.current) ImGui::TextUnformatted("Scene changed / snapshot stale: trail hidden");
            ImGui::Text("step: %s", a_s.step.c_str());
            ImGui::Text("footprints here %zu, readable %zu, clear %zu%s", a_s.here, a_s.readable, a_s.clear,
                a_s.badWeather ? "  (rain/snow: fading faster)" : "");
            ImGui::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "grey < %.2f", a_s.faint);
            ImGui::SameLine();
            ImGui::TextColored({ 0.94f, 0.78f, 0.16f, 1.0f }, "yellow: up close");
            ImGui::SameLine();
            ImGui::TextColored({ 0.24f, 0.86f, 0.35f, 1.0f }, "green >= %.2f", a_s.clearVis);
            ImGui::Text("trackers: %zu", a_s.trackers.size());
            ImGui::Separator();
            ImGui::Text("Scent stage 3: %s | active sources %zu | visual-only %zu",
                a_s.scent.enabled ? "ON" : "OFF", a_s.scented, a_s.legacy);
            ImGui::Text("Amount half-life %.2f game h | base %.2f | blood +%.2f | threshold %.3f",
                a_s.scent.halflife_hours, a_s.scent.base_strength, a_s.scent.blood_bonus, a_s.scent.min_strength);
            ImGui::TextUnformatted("Cyan: scent. Red rim: blood. Links never cross scent segments.");
            ImGui::Text("Spread limit x%.2f | time constant %.2f game h | peak also falls by dilution",
                a_s.scent.spread_max_mult, a_s.scent.spread_hours);
            ImGui::TextUnformatted("Faint ring: reference spread x1. Bright: selected NPC threshold range (else x1).");
            ImGui::TextUnformatted("A strong nose may reach beyond the x1 reference ring. Up to 24 sources, spaced.");
            ImGui::TextUnformatted("Age uses game hours. No wind. Geometry is not applied to these rings.");
            ImGui::TextUnformatted("NPC scent: no FOV/darkness; strict barrier ray; same segment while pursuing.");
            ImGui::TextUnformatted("Num1 in SCENT mode: force scent pickup for selected NPC (still needs a local source).");
            for (const auto& line : a_s.reader) ImGui::TextUnformatted(line.c_str());
            ImGui::End();
        }

        bool InitImGui(IDXGISwapChain* a_swapChain, float a_height) {
            if (FAILED(a_swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) || !g_device) {
                return false;
            }
            g_device->GetImmediateContext(&g_context);
            ImGui::CreateContext();
            auto& io       = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            // The built-in pixel font only looks right at whole multiples of 13 px
            ImFontConfig font;
            font.SizePixels = 13.0f * std::max(1.0f, std::round(a_height / 1080.0f));
            io.Fonts->AddFontDefault(&font);
            if (!ImGui_ImplDX11_Init(g_device, g_context)) {
                return false;
            }
            g_lastFrame = std::chrono::steady_clock::now();
            SKSE::log::info("Overlay: ImGui on the swap chain, font {:.0f} px", font.SizePixels);
            return true;
        }

        void Draw(IDXGISwapChain* a_swapChain) {
            DXGI_SWAP_CHAIN_DESC desc{};
            if (FAILED(a_swapChain->GetDesc(&desc)) || desc.BufferDesc.Width == 0 || desc.BufferDesc.Height == 0) {
                return;
            }
            const auto width  = static_cast<float>(desc.BufferDesc.Width);
            const auto height = static_cast<float>(desc.BufferDesc.Height);
            if (!g_imgui) {
                g_imgui = InitImGui(a_swapChain, height);
                if (!g_imgui) {
                    g_failed = true;
                    SKSE::log::error("Overlay: could not set up ImGui on the swap chain, overlay off");
                    return;
                }
            }

            Snapshot snapshot;
            {
                std::scoped_lock lock(g_lock);
                snapshot = g_snapshot;
            }
            Projector project;
            if (auto* camera = RE::Main::WorldRootCamera()) {
                std::memcpy(project.matrix, camera->GetRuntimeData().worldToCam, sizeof(project.matrix));
                project.port   = camera->GetRuntimeData2().port;
                project.width  = width;
                project.height = height;
                project.ok     = true;
            }
            const auto* player    = RE::PlayerCharacter::GetSingleton();
            const auto  playerPos = player ? player->GetPosition() : RE::NiPoint3{};

            auto&      io  = ImGui::GetIO();
            const auto now = std::chrono::steady_clock::now();
            auto* ui = RE::UI::GetSingleton();
            snapshot.current = player && player->Is3DLoaded() && Trail::SpaceOf(player) == snapshot.space &&
                ((ui && ui->GameIsPaused()) || std::chrono::duration<float>(now - snapshot.sampledAt).count() <=
                    std::max(3.0f, Config::Get().tracker.interval_ms / 500.0f));
            io.DisplaySize = { width, height };
            io.DeltaTime   = std::clamp(std::chrono::duration<float>(now - g_lastFrame).count(), 1e-4f, 0.25f);
            g_lastFrame    = now;

            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            DrawScene(snapshot, project, playerPos, g_mode.load());
            ImGui::Render();

            // Into the back buffer; the view is made per frame so the game can still resize the
            // buffers (a view held across frames would make ResizeBuffers fail)
            ID3D11Texture2D* back = nullptr;
            if (FAILED(a_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
                return;
            }
            ID3D11RenderTargetView* view = nullptr;
            const auto              hr   = g_device->CreateRenderTargetView(back, nullptr, &view);
            back->Release();
            if (FAILED(hr) || !view) {
                return;
            }
            ID3D11RenderTargetView* prevView  = nullptr;
            ID3D11DepthStencilView* prevDepth = nullptr;
            g_context->OMGetRenderTargets(1, &prevView, &prevDepth);
            g_context->OMSetRenderTargets(1, &view, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_context->OMSetRenderTargets(1, &prevView, prevDepth);
            if (prevView) {
                prevView->Release();
            }
            if (prevDepth) {
                prevDepth->Release();
            }
            view->Release();
        }

        HRESULT STDMETHODCALLTYPE Present(IDXGISwapChain* a_swapChain, UINT a_syncInterval, UINT a_flags) {
            if (g_mode.load() != 0 && !g_failed) {
                Draw(a_swapChain);
            }
            return g_present(a_swapChain, a_syncInterval, a_flags);
        }
    }

    void Install() {
        // [CL] BSGraphics::Renderer::GetCurrentRenderWindow; the game has one swap chain
        const auto* window    = RE::BSGraphics::Renderer::GetCurrentRenderWindow();
        auto*       swapChain = window ? reinterpret_cast<IDXGISwapChain*>(window->swapChain) : nullptr;
        if (!swapChain) {
            SKSE::log::warn("Overlay: no swap chain, overlay off");
            return;
        }
        // IDXGISwapChain::Present is vtable slot 8 (IUnknown 3, IDXGIObject 4, GetDevice 1). The
        // vtable belongs to whoever made the swap chain (dxgi, or an ENB/ReShade wrapper).
        auto**      vtable   = *reinterpret_cast<void***>(swapChain);
        void*       original = vtable[8];
        const void* hook     = reinterpret_cast<const void*>(&Present);
        g_present            = reinterpret_cast<PresentFn>(original);
        if (!REL::safe_write(reinterpret_cast<std::uintptr_t>(&vtable[8]), &hook, sizeof(hook), &original, sizeof(original))) {
            g_present = nullptr;
            SKSE::log::error("Overlay: could not hook Present, overlay off");
            return;
        }
        SKSE::log::info("Overlay: Present hooked, toggle with key {:#x}", Config::Get().debug.key_overlay);
    }

    void Toggle() {
        const auto mode = (g_mode.load() + 1) % 4;
        const bool on = mode != 0;
        if (on && !g_present) {
            RE::SendHUDMessage::ShowHUDMessage("StealthSenses: overlay is not available, see the log");
            return;
        }
        g_mode = mode;
        if (on) {
            Publish();  // a picture right away, not after the next tick
        } else {
            // Turning on again must not flash the old picture
            std::scoped_lock lock(g_lock);
            g_snapshot = {};
        }
        constexpr const char* messages[]{ "StealthSenses: overlay off", "StealthSenses: visual trail",
            "StealthSenses: scent", "StealthSenses: visual + scent" };
        RE::SendHUDMessage::ShowHUDMessage(messages[mode]);
    }

    bool ScentOnly() { return g_mode.load() == 2; }

    void Publish() {
        if (g_mode.load() == 0) {
            return;
        }
        auto* player   = RE::PlayerCharacter::GetSingleton();
        auto* calendar = RE::Calendar::GetSingleton();
        if (!player || !calendar) {
            return;
        }
        const auto  space    = Trail::SpaceOf(player);
        const auto  pos      = player->GetPosition();
        const float now      = calendar->GetHoursPassed();
        const auto* cell     = player->GetParentCell();
        const auto* sky      = RE::Sky::GetSingleton();
        const auto& tracker  = Config::Get().tracker;

        Snapshot s;
        s.space        = space;
        s.sampledAt    = std::chrono::steady_clock::now();
        s.scent        = Config::Get().scent;
        RE::Actor* reader = nullptr;
        if (auto ref = RE::Console::GetSelectedRef(); ref) reader = ref->As<RE::Actor>();
        if (!reader) {
            if (auto* pick = RE::CrosshairPickData::GetSingleton())
                if (auto ref = pick->GetActiveTarget().get(); ref) reader = ref->As<RE::Actor>();
        }
        std::optional<Profiles::Profile> selectedProfile;
        if (reader && reader != player) {
            const auto profile = Profiles::For(reader);
            if (Trail::SpaceOf(reader) == space) selectedProfile = profile;
            s.reader.push_back(std::format("Selected: {} [{:08X}]", reader->GetName(), reader->GetFormID()));
            s.reader.push_back(std::format("Visual {:.2f} | smell {:.2f} | blood smell x{:.2f} | calm hunt {}",
                profile.tracking, profile.smell, profile.smellBlood, profile.smellHunt ? "YES" : "NO"));
            const float regularR = Scent::Radius({s.scent.base_strength, s.scent.radius}, s.scent) * Scent::Reach(false, profile);
            const float bloodR = Scent::Radius({s.scent.base_strength + s.scent.blood_bonus, s.scent.radius}, s.scent) * Scent::Reach(true, profile);
            s.reader.push_back(std::format("Fresh open-space range: ordinary {:.0f} | blood {:.0f}", regularR, bloodR));
            const Trail::Footprint* best = nullptr;
            float strongest = 0, bestDist = 0;
            for (const auto& fp : Trail::Footprints()) {
                if (fp.space != Trail::SpaceOf(reader) || std::abs(fp.pos.z - reader->GetPosition().z) > tracker.max_height_diff) continue;
                const float dist = reader->GetPosition().GetDistance(fp.pos);
                const float strength = Scent::Perceived(Scent::SampleAt(fp, now, s.scent), dist, fp.flags & Trail::kBlood, profile, s.scent);
                if (strength > strongest) { strongest = strength; best = &fp; bestDist = dist; }
            }
            s.reader.push_back(best ? std::format("Strongest potential #{} seg{} dist{:.0f} S{:.3f} {} (before barrier/motivation)",
                best->seq, best->scentSegment, bestDist, strongest, strongest >= s.scent.min_strength ? "IN RANGE" : "TOO FAINT") :
                "No local scent potential for this NPC (profile/source/range/space).");
        } else s.reader.emplace_back("Select an NPC in the console for its scent profile and potential range.");
        s.badWeather   = cell && !cell->IsInteriorCell() && sky && (sky->IsRaining() || sky->IsSnowing());
        s.faint        = tracker.faint_visibility;
        s.clearVis     = tracker.min_visibility;
        s.searchRadius = tracker.search_radius;
        s.step         = Trail::DescribeStep(player);

        const Trail::Footprint* prev = nullptr;
        for (const auto& fp : Trail::Footprints()) {
            if (fp.space != space) {
                prev = nullptr;
                continue;
            }
            const float vis = Trail::Visibility(fp, now, s.badWeather);
            const auto sample = Scent::SampleAt(fp, now, s.scent);
            const float scent = sample.peak;
            ++s.here;
            s.scented += scent >= s.scent.min_strength;
            s.legacy += fp.scentSegment == 0;
            s.readable += vis >= s.faint;
            s.clear += vis >= s.clearVis;
            if (fp.pos.GetDistance(pos) > kRadius) {
                prev = nullptr; // do not connect displayed dots across a clipped-away break
                continue;
            }
            if (fp.pos.GetDistance(pos) <= kRadius) {
                const bool link = prev && prev->pos.GetDistance(fp.pos) <= kLinkDistance;
                Dot dot{ fp.pos, vis, fp.step, fp.seq, fp.material, (fp.flags & Trail::kBlood) != 0, link };
                dot.scent = scent;
                dot.age = std::max(now - fp.gameHours, 0.0f);
                dot.segment = fp.scentSegment;
                dot.spreadRadius = sample.radius;
                dot.scentRadius = Scent::Radius(sample, s.scent);
                if (selectedProfile) dot.readerRadius = dot.scentRadius * Scent::Reach(fp.flags & Trail::kBlood, *selectedProfile);
                dot.scentLink = link && Scent::Connected(*prev, fp) && scent >= s.scent.min_strength &&
                    Scent::Strength(*prev, now, s.scent) >= s.scent.min_strength;
                s.dots.push_back(dot);
            }
            prev = &fp;
        }
        std::vector<std::size_t> ringOrder;
        for (std::size_t i = 0; i < s.dots.size(); ++i) if (s.dots[i].scentRadius > 0) ringOrder.push_back(i);
        std::ranges::sort(ringOrder, [&](auto a, auto b) { return s.dots[a].pos.GetDistance(pos) < s.dots[b].pos.GetDistance(pos); });
        std::vector<std::size_t> rings;
        const float ringSpacing = std::clamp(s.scent.radius * 0.45f, 100.0f, 250.0f);
        for (const auto index : ringOrder) {
            if (rings.size() == 24) break;
            if (std::ranges::any_of(rings, [&](auto other) { return s.dots[index].pos.GetDistance(s.dots[other].pos) < ringSpacing; })) continue;
            s.dots[index].ring = true;
            rings.push_back(index);
        }
        for (const auto& mark : g_marks) {
            if (mark.space == space && mark.pos.GetDistance(pos) <= kRadius) {
                s.pins.push_back({ mark.pos, mark.number });
            }
        }
        s.trackers = Trackers::DebugViews();

        std::scoped_lock lock(g_lock);
        g_snapshot = std::move(s);
    }

    void NoteMark(int a_number) {
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            g_marks.push_back({ player->GetPosition(), Trail::SpaceOf(player), a_number });
        }
    }
}
