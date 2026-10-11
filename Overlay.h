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

// Dev build test tool: footprints, marks and trackers drawn over the frame (ImGui on the D3D11
// swap chain's Present), toggled with debug.key_overlay. Nothing is placed in the world or saved.
// The task threads publish a snapshot of the plugin state; Present only reads that copy.
// Compiled out of release: the calls below are empty there.
namespace StealthSenses::Overlay {
#ifdef STEALTHSENSES_DEV
    // On kDataLoaded: hooks Present of the game's swap chain
    void Install();
    // From a task (the input sink queues it): show or hide
    void Toggle();
    // From the tick task: copies footprints, marks and trackers around the player
    void Publish();
    // From the mark task: a mark pin at the player's position
    void NoteMark(int a_number);
#else
    inline void Install() {}
    inline void Toggle() {}
    inline void Publish() {}
    inline void NoteMark(int) {}
#endif
}
