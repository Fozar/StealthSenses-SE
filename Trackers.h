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

// NPC trackers: suspicious NPCs that notice the player's trail walk it footprint by
// footprint. Steering: a runtime XMarker linked to the NPC with LinkCustom02/03 and the
// vanilla defaultTravelToLinkCustom* package. Game-thread only.
namespace StealthSenses::Trackers {
    // Engine state a tracker changes on an NPC; saved so a loaded game can undo it.
    struct Binding {
        RE::FormID actor      = 0;
        RE::FormID marker     = 0;
        RE::FormID prevLinked = 0;  // what the NPC was linked to with our keyword before
    };

    // A victim of the player, remembered so its own who see the body look for the trail.
    struct KnownBody {
        RE::FormID corpse = 0;
        float      diedAt = 0.0f;  // Calendar::GetHoursPassed() at death
    };
    using BodySeen = std::pair<RE::FormID, RE::FormID>;  // (NPC, corpse): already reacted

    // Looks up the vanilla package, keyword and XMarker; needs data loaded.
    bool Init();

    // Called every Config tracker.interval_ms of unpaused play.
    void Update(float a_deltaSeconds);

    // From TESDeathEvent (via AddTask): remembers the body if the player, its follower or its
    // summon was the killer, so NPCs of the same faction who see it become suspicious.
    void NoteKill(RE::FormID a_corpse, RE::FormID a_killer);

    // Forgets trackers without touching the engine (revert/load: actors may be gone).
    void Clear();

    std::vector<Binding> Bindings();

    // Co-save: the player's victims and who already reacted to them.
    std::vector<KnownBody> Bodies();
    std::vector<BodySeen>  BodiesSeen();
    void                   RestoreBodies(std::vector<KnownBody> a_bodies, std::vector<BodySeen> a_seen);

    // Bindings read from the co-save; undone by ReleaseStale once the game is loaded.
    void QueueStale(std::vector<Binding> a_bindings);
    void ReleaseStale();
}
