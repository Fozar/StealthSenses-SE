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
#include "Trackers.h"
#include "Trail.h"

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
        };

        struct Pin {
            RE::NiPoint3 pos;
            int          number = 0;
        };

        struct Snapshot {
            std::vector<Dot>                 dots;
            std::vector<Pin>                 pins;
            std::vector<Trackers::DebugView> trackers;
            std::string                      step;
            std::size_t                      here = 0, readable = 0, clear = 0;
            bool                             badWeather = false;
            float                            faint = 0.1f, clearVis = 0.2f, searchRadius = 700.0f;
        };

        struct Mark {
            RE::NiPoint3 pos;
            RE::FormID   space  = 0;
            int          number = 0;
        };

        std::atomic<bool> g_enabled{ false };
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

        void DrawScene(const Snapshot& a_s, const Projector& a_project, const RE::NiPoint3& a_player) {
            auto* list = ImGui::GetBackgroundDrawList();

            // Trail: lines first, dots over them
            ImVec2 prev{};
            bool   prevOk = false;
            for (const auto& dot : a_s.dots) {
                ImVec2     screen{};
                const bool ok = a_project(dot.pos, screen);
                if (ok && prevOk && dot.link) {
                    list->AddLine(prev, screen, IM_COL32(200, 200, 200, 90), 1.0f);
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
                list->AddCircleFilled(screen, radius, VisColor(dot.vis, a_s));
                if (dot.blood) {
                    list->AddCircle(screen, radius + 2.0f, IM_COL32(230, 30, 30, 255), 0, 2.0f);
                }
                if (dist <= kLabelRadius) {
                    Text(list, { screen.x + radius + 3.0f, screen.y - 7.0f }, IM_COL32(255, 255, 255, 230),
                        std::format("#{} {:.2f}", dot.seq, dot.vis));
                    if (dist <= kDetailRadius) {
                        Text(list, { screen.x + radius + 3.0f, screen.y + 6.0f }, IM_COL32(200, 200, 200, 210),
                            std::format("{} x{:.2f}", Trail::MaterialName(dot.material), dot.step));
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
            ImGui::Text("step: %s", a_s.step.c_str());
            ImGui::Text("footprints here %zu, readable %zu, clear %zu%s", a_s.here, a_s.readable, a_s.clear,
                a_s.badWeather ? "  (rain/snow: fading faster)" : "");
            ImGui::TextColored({ 0.6f, 0.6f, 0.6f, 1.0f }, "grey < %.2f", a_s.faint);
            ImGui::SameLine();
            ImGui::TextColored({ 0.94f, 0.78f, 0.16f, 1.0f }, "yellow: up close");
            ImGui::SameLine();
            ImGui::TextColored({ 0.24f, 0.86f, 0.35f, 1.0f }, "green >= %.2f", a_s.clearVis);
            ImGui::Text("trackers: %zu", a_s.trackers.size());
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
            io.DisplaySize = { width, height };
            io.DeltaTime   = std::clamp(std::chrono::duration<float>(now - g_lastFrame).count(), 1e-4f, 0.25f);
            g_lastFrame    = now;

            ImGui_ImplDX11_NewFrame();
            ImGui::NewFrame();
            DrawScene(snapshot, project, playerPos);
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
            if (g_enabled && !g_failed) {
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
        const bool on = !g_enabled.load();
        if (on && !g_present) {
            RE::SendHUDMessage::ShowHUDMessage("StealthSenses: overlay is not available, see the log");
            return;
        }
        g_enabled = on;
        if (on) {
            Publish();  // a picture right away, not after the next tick
        } else {
            // Turning on again must not flash the old picture
            std::scoped_lock lock(g_lock);
            g_snapshot = {};
        }
        RE::SendHUDMessage::ShowHUDMessage(on ? "StealthSenses: overlay on" : "StealthSenses: overlay off");
    }

    void Publish() {
        if (!g_enabled) {
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
            ++s.here;
            s.readable += vis >= s.faint;
            s.clear += vis >= s.clearVis;
            if (fp.pos.GetDistance(pos) <= kRadius) {
                const bool link = prev && prev->pos.GetDistance(fp.pos) <= kLinkDistance;
                s.dots.push_back({ fp.pos, vis, fp.step, fp.seq, fp.material, (fp.flags & Trail::kBlood) != 0, link });
            }
            prev = &fp;
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
