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

#include "ProfileLogic.h"

// 0.11: sense profiles from Data/SKSE/Plugins/StealthSensesProfiles.json, then every
// StealthSensesProfiles_*.json (other mods' additions) in name order. Game-thread only.
namespace StealthSenses::Profiles {
    // On kDataLoaded: reads the files and looks up the forms the rules name
    void Load();

    // The profile of this NPC now (factions change, so it is not cached; a few dozen rules)
    Profile For(RE::Actor* a_actor);

    // The most any profile multiplies a footprint's visibility by (tracking times blood, at least
    // 1): footprints fainter than faint_visibility over this can never be read by anyone
    float MaxBoost();
}
