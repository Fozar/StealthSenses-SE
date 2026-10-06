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

#include "TrailLogic.h"

namespace StealthSenses::Trail {
    namespace {
        float BaseVisibility(RE::MATERIAL_ID a_material, const Config::TrailSettings& a_cfg) {
            using M = RE::MATERIAL_ID;
            switch (a_material) {
            case M::kSnow:
            case M::kSnowStairs:
            case M::kMud:
            case M::kDirt:
            case M::kSand:
            case M::kAsh:
            case M::kGrass:
                return a_cfg.soft_visibility;
            case M::kGravel:
                return a_cfg.gravel_visibility;
            case M::kNone:
                return a_cfg.unknown_visibility;
            default:
                return a_cfg.hard_visibility;
            }
        }
    }

    float Visibility(const Footprint& a_fp, float a_nowHours, bool a_badWeather, const Config::TrailSettings& a_cfg) {
        float base = BaseVisibility(a_fp.material, a_cfg);
        if (a_fp.flags & kBlood) {
            base += a_cfg.blood_visibility_bonus;
        }
        float halflife = a_cfg.halflife_hours;
        if (a_badWeather) {
            halflife *= a_cfg.weather_halflife_mult;
        }
        const float age = std::max(a_nowHours - a_fp.gameHours, 0.0f);
        return base * std::exp2(-age / halflife);
    }

    void Prune(std::deque<Footprint>& a_trail, float a_nowHours, float a_minVisibility, const Config::TrailSettings& a_cfg) {
        // Bad weather lowers visibility only for now; the clear-weather value is the most a
        // footprint will ever show. Before 0.7.2 footprints stayed for ten halflives (~1 h of real
        // time) and filled max_footprints long after they faded.
        std::erase_if(a_trail, [&](const Footprint& a_fp) {
            return Visibility(a_fp, a_nowHours, false, a_cfg) < a_minVisibility;
        });
    }

    void Thin(std::deque<Footprint>& a_trail, const Config::TrailSettings& a_cfg) {
        // At 64 units apart 2048 footprints were ~10-20 min of walking, and the old trail was gone
        // before it faded. Trackers read up to lead_distance (900) ahead and across gaps of
        // gap_distance (1200), so a sparser old trail still leads them.
        const auto limit = static_cast<std::size_t>(std::max(a_cfg.max_footprints, 1));
        const auto half  = a_trail.size() / 2;

        std::deque<Footprint> thinned;
        const Footprint*      kept = nullptr;  // deque::push_back keeps references valid
        for (std::size_t i = 0; i < a_trail.size(); ++i) {
            const auto& fp = a_trail[i];
            // The last footprint of the older half stays so the thinned part joins the rest
            const bool keep = i + 1 >= half || !kept || kept->space != fp.space ||
                              kept->pos.GetDistance(fp.pos) >= a_cfg.thin_spacing;
            if (keep) {
                thinned.push_back(fp);
                kept = &thinned.back();
            }
        }
        a_trail = std::move(thinned);
        while (a_trail.size() > limit) {
            a_trail.pop_front();
        }
    }

    const Footprint* FindBySeq(const std::deque<Footprint>& a_trail, std::uint32_t a_seq) {
        const auto it = std::ranges::lower_bound(a_trail, a_seq, {}, &Footprint::seq);
        return it != a_trail.end() && it->seq == a_seq ? &*it : nullptr;
    }
}
