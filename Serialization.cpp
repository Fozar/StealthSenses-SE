#include "Serialization.h"

#include "Trackers.h"
#include "Trail.h"

namespace StealthSenses::Serialization {
    namespace {
        constexpr std::uint32_t kUniqueID     = 'SSNS';
        constexpr std::uint32_t kTrailRecord  = 'TRAL';
        constexpr std::uint32_t kTrailVersion = 1;

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

        void OnSave(SKSE::SerializationInterface* a_intfc) {
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

        void OnLoad(SKSE::SerializationInterface* a_intfc) {
            Trail::Clear();
            Trackers::Clear();

            std::uint32_t type, version, length;
            while (a_intfc->GetNextRecordInfo(type, version, length)) {
                if (type != kTrailRecord) {
                    continue;
                }
                if (version != kTrailVersion) {
                    SKSE::log::warn("Serialization: trail record version {} unsupported, skipped", version);
                    continue;
                }
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
