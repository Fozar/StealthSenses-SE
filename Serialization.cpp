#include "Serialization.h"

#include "Trackers.h"
#include "Trail.h"

namespace StealthSenses::Serialization {
    namespace {
        constexpr std::uint32_t kUniqueID       = 'SSNS';
        constexpr std::uint32_t kTrailRecord    = 'TRAL';
        constexpr std::uint32_t kTrailVersion   = 1;
        constexpr std::uint32_t kBindingRecord  = 'TRKB';
        constexpr std::uint32_t kBindingVersion = 1;

        // Fixed on-disk layout; never store pointers or handles
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

        struct SavedBinding {
            std::uint32_t actor;
            std::uint32_t marker;
            std::uint32_t prevLinked;
        };
        static_assert(sizeof(SavedBinding) == 12);

        void SaveTrail(SKSE::SerializationInterface* a_intfc) {
            const auto& trail = Trail::Footprints();
            if (!a_intfc->OpenRecord(kTrailRecord, kTrailVersion)) {
                SKSE::log::error("Serialization: cannot open trail record");
                return;
            }
            const auto count = static_cast<std::uint32_t>(trail.size());
            a_intfc->WriteRecordData(count);
            for (const auto& fp : trail) {
                const SavedFootprint saved{
                    fp.pos.x, fp.pos.y, fp.pos.z, fp.space, fp.gameHours,
                    static_cast<std::uint32_t>(fp.material), fp.flags
                };
                a_intfc->WriteRecordData(saved);
            }
            SKSE::log::info("Serialization: saved {} footprints", count);
        }

        // Trackers change linked refs and place markers; the save keeps those changes,
        // so the bindings go to the co-save and are undone after load.
        void SaveBindings(SKSE::SerializationInterface* a_intfc) {
            const auto bindings = Trackers::Bindings();
            if (!a_intfc->OpenRecord(kBindingRecord, kBindingVersion)) {
                SKSE::log::error("Serialization: cannot open binding record");
                return;
            }
            const auto count = static_cast<std::uint32_t>(bindings.size());
            a_intfc->WriteRecordData(count);
            for (const auto& b : bindings) {
                a_intfc->WriteRecordData(SavedBinding{ b.actor, b.marker, b.prevLinked });
            }
            if (count) {
                SKSE::log::info("Serialization: saved {} tracker bindings", count);
            }
        }

        void LoadTrail(SKSE::SerializationInterface* a_intfc) {
            std::uint32_t count = 0;
            if (a_intfc->ReadRecordData(count) != sizeof(count)) {
                SKSE::log::error("Serialization: truncated trail record");
                return;
            }
            std::uint32_t restored = 0;
            for (std::uint32_t i = 0; i < count; ++i) {
                SavedFootprint saved{};
                if (a_intfc->ReadRecordData(saved) != sizeof(saved)) {
                    SKSE::log::error("Serialization: truncated footprint {}/{}", i, count);
                    break;
                }
                // Load order may have changed since the save
                RE::FormID space = 0;
                if (!a_intfc->ResolveFormID(saved.space, space)) {
                    continue;
                }
                Trail::Footprint fp;
                fp.pos       = { saved.x, saved.y, saved.z };
                fp.space     = space;
                fp.gameHours = saved.gameHours;
                fp.material  = static_cast<RE::MATERIAL_ID>(saved.material);
                fp.flags     = saved.flags;
                Trail::Restore(fp);
                ++restored;
            }
            SKSE::log::info("Serialization: restored {}/{} footprints", restored, count);
        }

        void LoadBindings(SKSE::SerializationInterface* a_intfc) {
            std::uint32_t count = 0;
            if (a_intfc->ReadRecordData(count) != sizeof(count)) {
                SKSE::log::error("Serialization: truncated binding record");
                return;
            }
            std::vector<Trackers::Binding> bindings;
            for (std::uint32_t i = 0; i < count; ++i) {
                SavedBinding saved{};
                if (a_intfc->ReadRecordData(saved) != sizeof(saved)) {
                    SKSE::log::error("Serialization: truncated binding {}/{}", i, count);
                    break;
                }
                Trackers::Binding b;
                if (!a_intfc->ResolveFormID(saved.actor, b.actor)) {
                    continue;
                }
                // Markers are created refs (0xFF......); a failed resolve just skips the cleanup
                a_intfc->ResolveFormID(saved.marker, b.marker);
                if (saved.prevLinked) {
                    a_intfc->ResolveFormID(saved.prevLinked, b.prevLinked);
                }
                bindings.push_back(b);
            }
            Trackers::QueueStale(std::move(bindings));
        }

        void OnSave(SKSE::SerializationInterface* a_intfc) {
            SaveTrail(a_intfc);
            SaveBindings(a_intfc);
        }

        void OnLoad(SKSE::SerializationInterface* a_intfc) {
            Trail::Clear();
            Trackers::Clear();

            std::uint32_t type, version, length;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type == kTrailRecord && version == kTrailVersion) {
                    LoadTrail(a_intfc);
                } else if (type == kBindingRecord && version == kBindingVersion) {
                    LoadBindings(a_intfc);
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
