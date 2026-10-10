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

// Detection hook: the call to Actor::CalculateDetection inside DoDetectionJob. Raises the
// detection level suspicious NPCs get on the player (detection.*, 0.9). Dev build: also counts
// calls per thread (open_questions #3) and keeps the raw outputs for telemetry (#1).
// The hook runs on six engine workers at once: it reads only the published bonus list.

namespace StealthSenses::Detection {
    // At plugin load: patches the call site if detection.enabled (or the dev probe is on)
    void Install();

    // kDataLoaded and each game load: logs whether another plugin patched the call after Install
    void Verify();

    // The last CalculateDetection outputs of one observer on the player; meanings unknown (#1)
    struct PlayerSample {
        std::int32_t  value = 0;  // detectionValue
        std::uint8_t  unk04 = 0;
        std::uint8_t  unk05 = 0;
        std::uint32_t unk06 = 0;
        RE::NiPoint3  pos;
        float         unk08 = 0.0f;
        float         unk09 = 0.0f;
        float         unk10 = 0.0f;
        std::uint32_t ageMs = 0;  // since that call
    };
    // Any thread; nullopt without the probe or before the observer's first check of the player
    std::optional<PlayerSample> LastOnPlayer(RE::FormID a_source);

    // Tracker tick: the bonus each suspicious NPC gets on the player, replacing the previous list
    void PublishBonuses(std::vector<std::pair<RE::FormID, std::int32_t>> a_bonuses);

    // Dev build: every 5 s logs the calls per thread since the previous report
    void Report();
}
