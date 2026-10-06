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

#include "Trackers.h"
#include "Trail.h"

// The co-save record layouts. Templates over the interface: in the game it is
// SKSE::SerializationInterface, in the unit tests (tests/) a byte buffer with the same three
// calls — WriteRecordData(const T&), ReadRecordData(T&) -> bytes read, ResolveFormID(old, new&).
// The layouts are what players' saves hold: change one only with a new record version.
namespace StealthSenses::CoSave {
    // TRAL v1: count, then 25-byte footprints
    #pragma pack(push, 1)
    struct SavedFootprint {
        float         x, y, z;
        std::uint32_t space;
        float         gameHours;
        std::uint32_t material;
        std::uint8_t  flags;
    };
    #pragma pack(pop)
    static_assert(sizeof(SavedFootprint) == 25);

    // TRKB v1: count, then {actor, marker, prevLinked}
    struct SavedBinding {
        std::uint32_t actor;
        std::uint32_t marker;
        std::uint32_t prevLinked;
    };
    static_assert(sizeof(SavedBinding) == 12);

    template <class I>
    void WriteTrail(const I& a_intfc, const std::deque<Trail::Footprint>& a_trail) {
        a_intfc.WriteRecordData(static_cast<std::uint32_t>(a_trail.size()));
        for (const auto& fp : a_trail) {
            a_intfc.WriteRecordData(SavedFootprint{ fp.pos.x, fp.pos.y, fp.pos.z, fp.space, fp.gameHours,
                static_cast<std::uint32_t>(fp.material), fp.flags });
        }
    }

    // Footprints in saved order; seq is not saved (Trail::Restore numbers them anew). A space
    // that no longer resolves (load order changed) drops the footprint; a truncated record
    // gives what was read before the cut.
    template <class I>
    std::vector<Trail::Footprint> ReadTrail(const I& a_intfc) {
        std::vector<Trail::Footprint> result;
        std::uint32_t                 count = 0;
        if (a_intfc.ReadRecordData(count) != sizeof(count)) {
            SKSE::log::error("Serialization: truncated trail record");
            return result;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            SavedFootprint saved{};
            if (a_intfc.ReadRecordData(saved) != sizeof(saved)) {
                SKSE::log::error("Serialization: truncated footprint {}/{}", i, count);
                break;
            }
            RE::FormID space = 0;
            if (!a_intfc.ResolveFormID(saved.space, space)) {
                continue;
            }
            Trail::Footprint fp;
            fp.pos       = { saved.x, saved.y, saved.z };
            fp.space     = space;
            fp.gameHours = saved.gameHours;
            fp.material  = static_cast<RE::MATERIAL_ID>(saved.material);
            fp.flags     = saved.flags;
            result.push_back(fp);
        }
        return result;
    }

    template <class I>
    void WriteBindings(const I& a_intfc, const std::vector<Trackers::Binding>& a_bindings) {
        a_intfc.WriteRecordData(static_cast<std::uint32_t>(a_bindings.size()));
        for (const auto& b : a_bindings) {
            a_intfc.WriteRecordData(SavedBinding{ b.actor, b.marker, b.prevLinked });
        }
    }

    // A binding whose actor no longer resolves is dropped. Markers are created refs (0xFF......);
    // a marker or previous link that fails to resolve is left 0 — only that part of the cleanup
    // is skipped.
    template <class I>
    std::vector<Trackers::Binding> ReadBindings(const I& a_intfc) {
        std::vector<Trackers::Binding> result;
        std::uint32_t                  count = 0;
        if (a_intfc.ReadRecordData(count) != sizeof(count)) {
            SKSE::log::error("Serialization: truncated binding record");
            return result;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            SavedBinding saved{};
            if (a_intfc.ReadRecordData(saved) != sizeof(saved)) {
                SKSE::log::error("Serialization: truncated binding {}/{}", i, count);
                break;
            }
            Trackers::Binding b;
            if (!a_intfc.ResolveFormID(saved.actor, b.actor)) {
                continue;
            }
            if (!a_intfc.ResolveFormID(saved.marker, b.marker)) {
                b.marker = 0;
            }
            if (!saved.prevLinked || !a_intfc.ResolveFormID(saved.prevLinked, b.prevLinked)) {
                b.prevLinked = 0;
            }
            result.push_back(b);
        }
        return result;
    }

    // BODY v1: count, {corpse u32, diedAt f32}..., count, {npc u32, corpse u32}...
    struct Bodies {
        std::vector<Trackers::KnownBody> bodies;
        std::vector<Trackers::BodySeen>  seen;
    };

    template <class I>
    void WriteBodies(const I& a_intfc, const Bodies& a_bodies) {
        a_intfc.WriteRecordData(static_cast<std::uint32_t>(a_bodies.bodies.size()));
        for (const auto& b : a_bodies.bodies) {
            a_intfc.WriteRecordData(b.corpse);
            a_intfc.WriteRecordData(b.diedAt);
        }
        a_intfc.WriteRecordData(static_cast<std::uint32_t>(a_bodies.seen.size()));
        for (const auto& [npc, corpse] : a_bodies.seen) {
            a_intfc.WriteRecordData(npc);
            a_intfc.WriteRecordData(corpse);
        }
    }

    // Entries whose forms no longer resolve are dropped; a truncated record gives what was read.
    template <class I>
    Bodies ReadBodies(const I& a_intfc) {
        Bodies        result;
        std::uint32_t count = 0;
        if (a_intfc.ReadRecordData(count) != sizeof(count)) {
            SKSE::log::error("Serialization: truncated body record");
            return result;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            Trackers::KnownBody b;
            if (a_intfc.ReadRecordData(b.corpse) != sizeof(b.corpse) || a_intfc.ReadRecordData(b.diedAt) != sizeof(b.diedAt)) {
                SKSE::log::error("Serialization: truncated body {}/{}", i, count);
                return result;
            }
            if (a_intfc.ResolveFormID(b.corpse, b.corpse)) {
                result.bodies.push_back(b);
            }
        }
        if (a_intfc.ReadRecordData(count) != sizeof(count)) {
            SKSE::log::error("Serialization: truncated body reactions");
            return result;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            Trackers::BodySeen s;
            if (a_intfc.ReadRecordData(s.first) != sizeof(s.first) || a_intfc.ReadRecordData(s.second) != sizeof(s.second)) {
                SKSE::log::error("Serialization: truncated body reaction {}/{}", i, count);
                break;
            }
            if (a_intfc.ResolveFormID(s.first, s.first) && a_intfc.ResolveFormID(s.second, s.second)) {
                result.seen.push_back(s);
            }
        }
        return result;
    }
}
