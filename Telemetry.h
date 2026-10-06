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

#include <nlohmann/json.hpp>

// Structured trace for offline analysis: StealthSenses.trace.jsonl next to the log, one JSON
// object per line. Record types: player, npc, footprint, log, mark (see docs/DEVELOPMENT.md, 1.4).
// Game-thread only.
namespace StealthSenses::Telemetry {
    void Open();
    bool Enabled();

    // Adds "t" (real seconds since Open) and "gh" (game hours) and writes the line.
    void Write(nlohmann::json a_record);

    // Human-readable tracker event, mirrored from the log
    void Log(std::string_view a_text);

    // Player pressed the mark key: numbered mark with the player's state
    void Mark();

    nlohmann::json Vec(const RE::NiPoint3& a_pos);
    std::string    Hex(std::uint32_t a_value);
}
