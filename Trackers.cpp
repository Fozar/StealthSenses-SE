#include "Trackers.h"

#include "Config.h"
#include "Trail.h"

#include <unordered_set>

namespace StealthSenses::Trackers {
    namespace {
        struct State {
            std::uint32_t lastSeq   = 0;
            RE::NiPoint3  target;
            float         sinceEmit = 0.0f;
            bool          arrived   = false;
        };

        struct Pending {
            RE::Actor*             actor;
            const Trail::Footprint* fp;
            float                  waited;  // seconds since this tracker's last point
            int                    meter;
            bool                   alerted;
        };

        // Survives dropping a tracker: an NPC that lost the trail or saw the player does not
        // pick it up again right away, and never restarts from a footprint it already had.
        struct Memory {
            std::uint32_t lastSeq  = 0;
            float         cooldown = 0.0f;
        };

        std::unordered_map<RE::FormID, State>  g_trackers;
        std::unordered_map<RE::FormID, Memory> g_memory;

        void Remember(RE::FormID a_id, std::uint32_t a_lastSeq) {
            auto& memory    = g_memory[a_id];
            memory.lastSeq  = std::max(memory.lastSeq, a_lastSeq);
            memory.cooldown = Config::Get().tracker.repickup_seconds;
        }

        RE::SOUND_LEVEL ParseSoundLevel(std::string_view a_name) {
            if (a_name == "loud") return RE::SOUND_LEVEL::kLoud;
            if (a_name == "normal") return RE::SOUND_LEVEL::kNormal;
            if (a_name == "silent") return RE::SOUND_LEVEL::kSilent;
            if (a_name == "very_loud") return RE::SOUND_LEVEL::kVeryLoud;
            return RE::SOUND_LEVEL::kQuiet;
        }

        // RequestDetectionLevel returns < 0 while the target is undetected; MaxsuDetectionMeter
        // maps it to a 0..100 meter as level + 100, and any value >= 0 to 100 (detected).
        // repos/max-su-2019_MaxsuDetectionMeter/src/DataHandler.cpp:21-28 [prior art, not RE]
        int StealthMeter(RE::Actor* a_observer, RE::Actor* a_target) {
            const auto level = a_observer->RequestDetectionLevel(a_target);
            return level < 0 ? std::clamp(level + 100, 0, 100) : 100;
        }

        bool IsCandidate(RE::Actor* a_actor, RE::PlayerCharacter* a_player, RE::FormID a_space) {
            if (!a_actor || a_actor == a_player || a_actor->IsDead()) {
                return false;
            }
            if (a_actor->IsPlayerTeammate() || a_actor->IsCommandedActor()) {
                return false;
            }
            if (!a_actor->GetActorRuntimeData().currentProcess) {
                return false;
            }
            return Trail::SpaceOf(a_actor) == a_space;
        }

        void Say(const std::string& a_text) {
            SKSE::log::info("{}", a_text);
            if (Config::Get().debug.console) {
                if (auto* console = RE::ConsoleLog::GetSingleton()) {
                    console->Print("[StealthSenses] %s", a_text.c_str());
                }
            }
        }

        std::string Describe(RE::Actor* a_actor) {
            const char* name = a_actor->GetName();
            return std::format("{} [{:08X}]", name && *name ? name : "?", a_actor->GetFormID());
        }

        void Emit(RE::PlayerCharacter* a_player, const Pending& a_pending) {
            auto* process = a_player->GetActorRuntimeData().currentProcess;
            if (!process) {
                return;
            }
            const auto& cfg   = Config::Get().tracker;
            const auto  sound = RE::AIFormulas::GetSoundLevelValue(ParseSoundLevel(cfg.sound_level));

            // [engine_api §2] the event belongs to the source (player): one point at a time
            // for all listeners, which is why Update emits for a single tracker per tick.
            process->SetActorsDetectionEvent(a_player, a_pending.fp->pos, sound, nullptr);

            auto& state     = g_trackers[a_pending.actor->GetFormID()];
            const bool first = state.lastSeq == 0;
            state.lastSeq   = a_pending.fp->seq;
            state.target    = a_pending.fp->pos;
            state.sinceEmit = 0.0f;
            state.arrived   = false;

            Say(std::format("{} {} footprint #{} dist {:.0f} meter {} alert {} sound {}",
                Describe(a_pending.actor), first ? "picked up trail at" : "follows to",
                a_pending.fp->seq, a_pending.actor->GetPosition().GetDistance(a_pending.fp->pos),
                a_pending.meter, a_pending.alerted, sound));
        }
    }

    void Update(float a_deltaSeconds) {
        const auto& cfg = Config::Get().tracker;
        auto* player    = RE::PlayerCharacter::GetSingleton();
        auto* processes = RE::ProcessLists::GetSingleton();
        auto* calendar  = RE::Calendar::GetSingleton();
        auto* sky       = RE::Sky::GetSingleton();
        if (!cfg.enabled || !player || !processes || !calendar) {
            return;
        }

        const auto space = Trail::SpaceOf(player);
        if (space == 0) {
            return;
        }

        const float now      = calendar->GetHoursPassed();
        const auto* cell     = player->GetParentCell();
        const bool  exterior = cell && !cell->IsInteriorCell();
        const bool  badWeather = exterior && sky && (sky->IsRaining() || sky->IsSnowing());

        Trail::Prune(now);

        std::vector<const Trail::Footprint*> readable;
        for (const auto& fp : Trail::Footprints()) {
            if (fp.space == space && Trail::Visibility(fp, now, badWeather) >= cfg.min_visibility) {
                readable.push_back(&fp);
            }
        }

        for (auto& [id, memory] : g_memory) {
            memory.cooldown = std::max(memory.cooldown - a_deltaSeconds, 0.0f);
        }

        std::unordered_set<RE::FormID> seen;
        std::vector<Pending>           pending;

        processes->ForEachHighActor([&](RE::Actor* a_actor) {
            if (!IsCandidate(a_actor, player, space)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id      = a_actor->GetFormID();
            const auto tracked = g_trackers.find(id);

            if (a_actor->IsInCombat() && !cfg.include_combat) {
                if (tracked != g_trackers.end()) {
                    Say(std::format("{} entered combat, stops tracking", Describe(a_actor)));
                    Remember(id, tracked->second.lastSeq);
                    g_trackers.erase(tracked);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (cfg.require_hostile && !a_actor->IsHostileToActor(player)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            const int  meter   = StealthMeter(a_actor, player);
            const bool alerted = a_actor->GetActorRuntimeData().currentProcess->lowProcessFlags.all(
                RE::AIProcess::LowProcessFlags::kAlert);

            if (meter >= 100) {
                // Sees the player: vanilla detection takes over
                if (tracked != g_trackers.end()) {
                    Say(std::format("{} detected the player, stops tracking", Describe(a_actor)));
                    Remember(id, tracked->second.lastSeq);
                    g_trackers.erase(tracked);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

            seen.insert(id);
            const auto pos = a_actor->GetPosition();

            if (tracked == g_trackers.end()) {
                const bool suspicious = cfg.debug_all_npcs || alerted || meter >= cfg.caution_level;
                if (!suspicious || g_trackers.size() >= static_cast<std::size_t>(cfg.max_trackers)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto memory  = g_memory.find(id);
                const auto skipSeq = memory != g_memory.end() ? memory->second.lastSeq : 0;
                if (memory != g_memory.end() && memory->second.cooldown > 0.0f) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                // Picks up the trail at the nearest readable footprint it has not had yet
                const Trail::Footprint* best  = nullptr;
                float                   bestD = cfg.search_radius;
                for (const auto* fp : readable) {
                    if (fp->seq <= skipSeq) {
                        continue;
                    }
                    const float d = pos.GetDistance(fp->pos);
                    if (d <= bestD) {
                        bestD = d;
                        best  = fp;
                    }
                }
                if (best) {
                    pending.push_back({ a_actor, best, std::numeric_limits<float>::max(), meter, alerted });
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

            auto& state = tracked->second;
            state.sinceEmit += a_deltaSeconds;

            const float toTarget = pos.GetDistance(state.target);
            SKSE::log::trace("{} -> #{}: {:.0f} away after {:.1f}s", Describe(a_actor), state.lastSeq, toTarget, state.sinceEmit);
            if (!state.arrived && toTarget <= cfg.arrive_radius) {
                state.arrived = true;
                SKSE::log::info("{} arrived at #{} after {:.1f}s", Describe(a_actor), state.lastSeq, state.sinceEmit);
            }
            if (!state.arrived && state.sinceEmit < cfg.retarget_seconds) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            // Next point: the freshest readable footprint ahead on the trail within reach
            const Trail::Footprint* next = nullptr;
            for (const auto* fp : readable) {
                if (fp->seq > state.lastSeq && pos.GetDistance(fp->pos) <= cfg.lead_distance &&
                    (!next || fp->seq > next->seq)) {
                    next = fp;
                }
            }
            if (next) {
                if (!state.arrived) {
                    SKSE::log::info("{} did not reach #{} in {:.1f}s (still {:.0f} away)",
                        Describe(a_actor), state.lastSeq, state.sinceEmit, toTarget);
                }
                pending.push_back({ a_actor, next, state.sinceEmit, meter, alerted });
            } else if (state.sinceEmit >= cfg.lost_seconds) {
                Say(std::format("{} lost the trail after #{}", Describe(a_actor), state.lastSeq));
                Remember(id, state.lastSeq);
                seen.erase(id);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        std::erase_if(g_trackers, [&](const auto& a_entry) { return !seen.contains(a_entry.first); });

        if (!pending.empty()) {
            const auto& due = *std::ranges::max_element(pending, {}, &Pending::waited);
            Emit(player, due);
        }
    }

    void LogSoundLevels() {
        using S = RE::SOUND_LEVEL;
        SKSE::log::info("Sound level values: loud {} normal {} silent {} very_loud {} quiet {}",
            RE::AIFormulas::GetSoundLevelValue(S::kLoud), RE::AIFormulas::GetSoundLevelValue(S::kNormal),
            RE::AIFormulas::GetSoundLevelValue(S::kSilent), RE::AIFormulas::GetSoundLevelValue(S::kVeryLoud),
            RE::AIFormulas::GetSoundLevelValue(S::kQuiet));
    }

    void Clear() {
        g_trackers.clear();
        g_memory.clear();
    }
}
