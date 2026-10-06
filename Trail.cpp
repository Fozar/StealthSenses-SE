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

#include "Trail.h"

#include "Config.h"
#include "Telemetry.h"

namespace StealthSenses::Trail {
    namespace {
        std::deque<Footprint> g_trail;
        std::uint32_t         g_nextSeq    = 1;
        int                   g_bloodSteps = 0;

        using Clock = std::chrono::steady_clock;
        Clock::time_point g_lastFootstepEvent{};

        float BaseVisibility(RE::MATERIAL_ID a_material) {
            using M = RE::MATERIAL_ID;
            const auto& t = Config::Get().trail;
            switch (a_material) {
            case M::kSnow:
            case M::kSnowStairs:
            case M::kMud:
            case M::kDirt:
            case M::kSand:
            case M::kAsh:
            case M::kGrass:
                return t.soft_visibility;
            case M::kGravel:
                return t.gravel_visibility;
            case M::kNone:
                return t.unknown_visibility;
            default:
                return t.hard_visibility;
            }
        }

        RE::MATERIAL_ID SurfaceUnder(RE::PlayerCharacter* a_player) {
            // [engine_api §6] bhkCharacterController::surfaceMaterial, works indoors too
            auto material = RE::MATERIAL_ID::kNone;
            if (const auto* controller = a_player->GetCharController()) {
                material = controller->surfaceMaterial;
            }
            // On exterior landscape the controller mostly reports kNone (2nd in-game test:
            // 384 of 547 footprints). Fall back to the land texture material, as DTD does:
            // repos/maglarnet_DTD/src/SurfaceProfiles.cpp:658, TES::GetLandMaterialType 13203/13349
            if (material == RE::MATERIAL_ID::kNone) {
                const auto* cell = a_player->GetParentCell();
                const auto* tes  = RE::TES::GetSingleton();
                if (cell && !cell->IsInteriorCell() && tes) {
                    material = tes->GetLandMaterialType(a_player->GetPosition());
                }
            }
            return material;
        }
    }

    RE::FormID SpaceOf(const RE::TESObjectREFR* a_ref) {
        if (!a_ref) {
            return 0;
        }
        const auto* cell = a_ref->GetParentCell();
        if (!cell) {
            return 0;
        }
        if (cell->IsInteriorCell()) {
            return cell->GetFormID();
        }
        const auto* world = a_ref->GetWorldspace();
        return world ? world->GetFormID() : 0;
    }

    void TryRecord(RE::PlayerCharacter* a_player, std::string_view a_source) {
        const auto& cfg = Config::Get().trail;
        if (!cfg.enabled || !a_player) {
            return;
        }

        const auto space = SpaceOf(a_player);
        if (space == 0) {
            return;
        }

        // Water breaks the trail; mounts are out of scope for the prototype
        if (const auto* state = a_player->AsActorState(); state && state->IsSwimming()) {
            return;
        }
        if (a_player->IsOnMount()) {
            return;
        }

        const auto pos = a_player->GetPosition();
        if (!g_trail.empty()) {
            const auto& last = g_trail.back();
            if (last.space == space && last.pos.GetDistance(pos) < cfg.min_spacing) {
                return;
            }
        }

        const auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return;
        }

        // Wading through shallow water (not swimming) washes footprints away too
        const auto material = SurfaceUnder(a_player);
        if (material == RE::MATERIAL_ID::kWater) {
            return;
        }

        Footprint fp;
        fp.pos       = pos;
        fp.space     = space;
        fp.gameHours = calendar->GetHoursPassed();
        fp.material  = material;
        fp.seq       = g_nextSeq++;
        if (g_bloodSteps > 0) {
            fp.flags |= kBlood;
            --g_bloodSteps;
        }

        g_trail.push_back(fp);
        while (g_trail.size() > static_cast<std::size_t>(cfg.max_footprints)) {
            g_trail.pop_front();
        }

        Telemetry::Write({ { "type", "footprint" }, { "seq", fp.seq }, { "pos", Telemetry::Vec(pos) },
            { "space", Telemetry::Hex(space) }, { "mat", Telemetry::Hex(static_cast<std::uint32_t>(fp.material)) },
            { "base", Visibility(fp, fp.gameHours, false) }, { "blood", (fp.flags & kBlood) != 0 },
            { "src", a_source } });

        SKSE::log::trace("Trail: #{} via {} at ({:.0f}, {:.0f}, {:.0f}) space {:08X} material {:08X}{}",
            fp.seq, a_source, pos.x, pos.y, pos.z, space,
            static_cast<std::uint32_t>(fp.material), (fp.flags & kBlood) ? " blood" : "");
    }

    void MarkPlayerHit() {
        g_bloodSteps = Config::Get().trail.blood_steps;
    }

    float Visibility(const Footprint& a_fp, float a_nowHours, bool a_badWeather) {
        const auto& cfg = Config::Get().trail;
        float base = BaseVisibility(a_fp.material);
        if (a_fp.flags & kBlood) {
            base += cfg.blood_visibility_bonus;
        }
        float halflife = cfg.halflife_hours;
        if (a_badWeather) {
            halflife *= cfg.weather_halflife_mult;
        }
        const float age = std::max(a_nowHours - a_fp.gameHours, 0.0f);
        return base * std::exp2(-age / halflife);
    }

    void Prune(float a_nowHours) {
        // Ten halflives: even a bloody footprint is below 0.2% of its base visibility
        const float maxAge = Config::Get().trail.halflife_hours * 10.0f;
        while (!g_trail.empty() && a_nowHours - g_trail.front().gameHours > maxAge) {
            g_trail.pop_front();
        }
    }

    const std::deque<Footprint>& Footprints() {
        return g_trail;
    }

    void Clear() {
        g_trail.clear();
        g_bloodSteps = 0;
    }

    void Restore(const Footprint& a_fp) {
        auto fp = a_fp;
        fp.seq  = g_nextSeq++;
        g_trail.push_back(fp);
    }

    bool FootstepEventsActive() {
        return Clock::now() - g_lastFootstepEvent < 1s;
    }

    void NoteFootstepEvent() {
        g_lastFootstepEvent = Clock::now();
    }
}
