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

#pragma once

#include "Config.h"
#include "Trail.h"

// The trail's pure rules: no game state, settings passed in. Trail.cpp applies them to the live
// trail; the unit tests (tests/) call them directly.
namespace StealthSenses::Trail {
    // Visibility in [0, ~2]: base by material (+blood) halved every halflife_hours of age; in
    // bad weather the halflife is shorter.
    float Visibility(const Footprint& a_fp, float a_nowHours, bool a_badWeather, const Config::TrailSettings& a_cfg);

    // Drops footprints below a_minVisibility even in clear weather: visibility only falls with
    // age, so they can never be read again. Leaves gaps in seq.
    void Prune(std::deque<Footprint>& a_trail, float a_nowHours, float a_minVisibility, const Config::TrailSettings& a_cfg);

    // Over max_footprints: the older half keeps one footprint per thin_spacing (a change of space
    // always kept); whatever is still over the limit goes oldest first.
    void Thin(std::deque<Footprint>& a_trail, const Config::TrailSettings& a_cfg);

    // seq grows along the trail, with gaps where Prune and Thin removed footprints.
    const Footprint* FindBySeq(const std::deque<Footprint>& a_trail, std::uint32_t a_seq);
}
