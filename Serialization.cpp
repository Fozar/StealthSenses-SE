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

#include "Serialization.h"

#include "CoSaveFormat.h"
#include "Trackers.h"
#include "Trail.h"

namespace StealthSenses::Serialization {
    namespace {
        constexpr std::uint32_t kUniqueID       = 'SSNS';
        constexpr std::uint32_t kTrailRecord    = 'TRAL';
        constexpr std::uint32_t kTrailVersion   = 1;
        constexpr std::uint32_t kBindingRecord  = 'TRKB';
        constexpr std::uint32_t kBindingVersion = 1;
        constexpr std::uint32_t kBodyRecord     = 'BODY';
        constexpr std::uint32_t kBodyVersion    = 1;

        // Record layouts are in CoSaveFormat.h (unit-tested); this file opens the records and
        // moves data between them and Trail / Trackers.
        void OnSave(SKSE::SerializationInterface* a_intfc) {
            if (a_intfc->OpenRecord(kTrailRecord, kTrailVersion)) {
                CoSave::WriteTrail(*a_intfc, Trail::Footprints());
                SKSE::log::info("Serialization: saved {} footprints", Trail::Footprints().size());
            } else {
                SKSE::log::error("Serialization: cannot open trail record");
            }
            // Trackers change linked refs and place markers; the save keeps those changes, so the
            // bindings go to the co-save and are undone after load.
            if (a_intfc->OpenRecord(kBindingRecord, kBindingVersion)) {
                CoSave::WriteBindings(*a_intfc, Trackers::Bindings());
            } else {
                SKSE::log::error("Serialization: cannot open binding record");
            }
            if (a_intfc->OpenRecord(kBodyRecord, kBodyVersion)) {
                CoSave::WriteBodies(*a_intfc, { Trackers::Bodies(), Trackers::BodiesSeen() });
            } else {
                SKSE::log::error("Serialization: cannot open body record");
            }
        }

        void OnLoad(SKSE::SerializationInterface* a_intfc) {
            Trail::Clear();
            Trackers::Clear();

            std::uint32_t type, version, length;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type == kTrailRecord && version == kTrailVersion) {
                    const auto footprints = CoSave::ReadTrail(*a_intfc);
                    for (const auto& fp : footprints) {
                        Trail::Restore(fp);
                    }
                    SKSE::log::info("Serialization: restored {} footprints", footprints.size());
                } else if (type == kBindingRecord && version == kBindingVersion) {
                    Trackers::QueueStale(CoSave::ReadBindings(*a_intfc));
                } else if (type == kBodyRecord && version == kBodyVersion) {
                    auto bodies = CoSave::ReadBodies(*a_intfc);
                    Trackers::RestoreBodies(std::move(bodies.bodies), std::move(bodies.seen));
                } else {
                    SKSE::log::warn("Serialization: record {:08X} v{} unsupported, skipped", type, version);
                }
            }
        }

        void OnRevert(SKSE::SerializationInterface*) {
            Trail::Clear();
            Trackers::Clear();
        }
    }

    void Register() {
        const auto* intfc = SKSE::GetSerializationInterface();
        intfc->SetUniqueID(kUniqueID);
        intfc->SetSaveCallback(OnSave);
        intfc->SetLoadCallback(OnLoad);
        intfc->SetRevertCallback(OnRevert);
    }
}
