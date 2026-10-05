#include "Trackers.h"

#include "Config.h"
#include "Trail.h"

#include <unordered_set>

namespace StealthSenses::Trackers {
    namespace {
        // Skyrim.esm forms, FormIDs read from the record headers of the local Skyrim.esm.
        // Both packages target PLDT type 6 (linked reference) with keyword LinkCustom02 (0005D5E7),
        // so one marker serves both:
        // defaultTravelToLinkCustom02 (template Travel 00016FAA) — walk to the footprint;
        // DefaultSandboxLinkCustom02512 (template Sandbox 0001C254) — wander within 512 of it.
        constexpr RE::FormID kTravelPackage = 0x000235E2;
        constexpr RE::FormID kSearchPackage = 0x000DD837;
        constexpr RE::FormID kKeyword       = 0x0005D5E7;
        constexpr RE::FormID kXMarker       = 0x0000003B;

        RE::TESPackage*     g_travel  = nullptr;
        RE::TESPackage*     g_search  = nullptr;
        RE::BGSKeyword*     g_keyword = nullptr;
        RE::TESBoundObject* g_xmarker = nullptr;
        bool                g_ready   = false;

        struct State {
            std::uint32_t                    lastSeq   = 0;
            RE::NiPoint3                     target;
            float                            sinceStep = 0.0f;
            bool                             arrived   = false;
            RE::NiPointer<RE::TESObjectREFR> marker;
            RE::FormID                       prevLinked  = 0;
            RE::TESPackage*                  intended    = nullptr;  // travel, or search once the trail runs out
            bool                             packageLost = false;
        };

        // Survives dropping a tracker: an NPC that lost the trail or saw the player does not
        // pick it up again right away, and never restarts from a footprint it already had.
        struct Memory {
            std::uint32_t lastSeq  = 0;
            float         cooldown = 0.0f;
        };

        std::unordered_map<RE::FormID, State>  g_trackers;
        std::unordered_map<RE::FormID, Memory> g_memory;
        std::vector<Binding>                   g_stale;

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

        void Remember(RE::FormID a_id, std::uint32_t a_lastSeq) {
            auto& memory    = g_memory[a_id];
            memory.lastSeq  = std::max(memory.lastSeq, a_lastSeq);
            memory.cooldown = Config::Get().tracker.repickup_seconds;
        }

        // Undoes what Steer did: linked ref back, marker gone, normal AI re-evaluated.
        void Release(RE::Actor* a_actor, RE::TESObjectREFR* a_marker, RE::FormID a_prevLinked) {
            if (a_actor && g_keyword) {
                auto* prev = a_prevLinked ? RE::TESForm::LookupByID<RE::TESObjectREFR>(a_prevLinked) : nullptr;
                // SetLinkedRef(nullptr, kw) is assumed to drop the entry [not verified]
                a_actor->extraList.SetLinkedRef(prev, g_keyword);
                a_actor->EvaluatePackage(true, false);
            }
            if (a_marker) {
                a_marker->Disable();
                a_marker->SetDelete(true);
            }
        }

        void Drop(RE::Actor* a_actor, State& a_state, std::string_view a_why) {
            Say(std::format("{} {} after #{}, stops tracking", Describe(a_actor), a_why, a_state.lastSeq));
            Remember(a_actor->GetFormID(), a_state.lastSeq);
            Release(a_actor, a_state.marker.get(), a_state.prevLinked);
        }

        // Arguments: temp package, not created by us (a vanilla form must not be freed),
        // allowed to pull the NPC out of furniture. Semantics are not verified [assumption].
        void Apply(RE::Actor* a_actor, RE::TESPackage* a_package) {
            a_actor->PutCreatedPackage(a_package, true, false, true);
        }

        // Sends the NPC to the footprint: marker on the footprint, NPC linked to it, travel package.
        // False if the marker could not be placed; the caller must not keep the state then.
        bool Steer(RE::PlayerCharacter* a_player, RE::Actor* a_actor, State& a_state, const Trail::Footprint& a_fp) {
            if (!a_state.marker) {
                a_state.marker = a_player->PlaceObjectAtMe(g_xmarker, false);
                if (!a_state.marker) {
                    SKSE::log::error("{}: PlaceObjectAtMe(XMarker) failed", Describe(a_actor));
                    return false;
                }
                const auto* prev   = a_actor->extraList.GetLinkedRef(g_keyword);
                a_state.prevLinked = prev ? prev->GetFormID() : 0;
                a_actor->extraList.SetLinkedRef(a_state.marker.get(), g_keyword);
            }
            a_state.marker->SetPosition(a_fp.pos);

            const bool first    = a_state.lastSeq == 0;
            a_state.lastSeq     = a_fp.seq;
            a_state.target      = a_fp.pos;
            a_state.sinceStep   = 0.0f;
            a_state.arrived     = false;
            a_state.packageLost = false;
            a_state.intended    = g_travel;

            Apply(a_actor, g_travel);

            const auto* current = a_actor->GetCurrentPackage();
            Say(std::format("{} {} footprint #{} dist {:.0f}, package {:08X}{}",
                Describe(a_actor), first ? "picked up trail at" : "follows to", a_fp.seq,
                a_actor->GetPosition().GetDistance(a_fp.pos), current ? current->GetFormID() : 0,
                current == g_travel ? "" : " (not ours!)"));
            return true;
        }

        // The trail ran out at the marker: wander around it instead of re-sending the finished
        // travel package (test 0.2.2: travel done -> own sleep package -> travel again, 16 s of dithering)
        void Search(RE::Actor* a_actor, State& a_state) {
            a_state.intended    = g_search;
            a_state.packageLost = false;
            Apply(a_actor, g_search);
            Say(std::format("{} searches around #{}", Describe(a_actor), a_state.lastSeq));
        }
    }

    bool Init() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            return false;
        }
        g_travel  = data->LookupForm<RE::TESPackage>(kTravelPackage, "Skyrim.esm");
        g_search  = data->LookupForm<RE::TESPackage>(kSearchPackage, "Skyrim.esm");
        g_keyword = data->LookupForm<RE::BGSKeyword>(kKeyword, "Skyrim.esm");
        // 0x3B is a hardcoded engine form below 0x800: LookupForm(…, "Skyrim.esm") returns
        // null for it (in-game test 0.2.0), the global lookup finds it
        if (auto* form = RE::TESForm::LookupByID(kXMarker)) {
            g_xmarker = form->As<RE::TESBoundObject>();
        }
        SKSE::log::info("Trackers: travel {:08X} search {:08X} keyword {:08X} xmarker {:08X}",
            g_travel ? g_travel->GetFormID() : 0, g_search ? g_search->GetFormID() : 0,
            g_keyword ? g_keyword->GetFormID() : 0, g_xmarker ? g_xmarker->GetFormID() : 0);
        g_ready = g_travel && g_search && g_keyword && g_xmarker;
        return g_ready;
    }

    void Update(float a_deltaSeconds) {
        const auto& cfg = Config::Get().tracker;
        auto* player    = RE::PlayerCharacter::GetSingleton();
        auto* processes = RE::ProcessLists::GetSingleton();
        auto* calendar  = RE::Calendar::GetSingleton();
        auto* sky       = RE::Sky::GetSingleton();
        if (!cfg.enabled || !g_ready || !player || !processes || !calendar) {
            return;
        }

        const auto space = Trail::SpaceOf(player);
        if (space == 0) {
            return;
        }

        const float now        = calendar->GetHoursPassed();
        const auto* cell       = player->GetParentCell();
        const bool  exterior   = cell && !cell->IsInteriorCell();
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

        processes->ForEachHighActor([&](RE::Actor* a_actor) {
            if (!IsCandidate(a_actor, player, space)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id      = a_actor->GetFormID();
            const auto tracked = g_trackers.find(id);

            if (a_actor->IsInCombat() && !cfg.include_combat) {
                if (tracked != g_trackers.end()) {
                    Drop(a_actor, tracked->second, "entered combat");
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
                    Drop(a_actor, tracked->second, "detected the player");
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
                const auto memory = g_memory.find(id);
                if (memory != g_memory.end() && memory->second.cooldown > 0.0f) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto skipSeq = memory != g_memory.end() ? memory->second.lastSeq : 0;

                // Notices only a footprint right next to it; follows the freshest one around
                const Trail::Footprint* best = nullptr;
                for (const auto* fp : readable) {
                    if (fp->seq > skipSeq && pos.GetDistance(fp->pos) <= cfg.notice_radius &&
                        (!best || fp->seq > best->seq)) {
                        best = fp;
                    }
                }
                if (best) {
                    SKSE::log::info("{} noticed footprint #{} (meter {}, alert {})", Describe(a_actor), best->seq, meter, alerted);
                    State state;
                    if (Steer(player, a_actor, state, *best)) {
                        g_trackers.emplace(id, std::move(state));
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

            auto& state = tracked->second;
            state.sinceStep += a_deltaSeconds;

            const auto* current  = a_actor->GetCurrentPackage();
            const float toTarget = pos.GetDistance(state.target);
            SKSE::log::trace("{} -> #{}: {:.0f} away after {:.1f}s, package {:08X}",
                Describe(a_actor), state.lastSeq, toTarget, state.sinceStep, current ? current->GetFormID() : 0);
            if (current != state.intended) {
                // Another package won the evaluation (seen in test 0.2.1: dynamic FF… packages
                // and the NPC's own patrol). Put ours back; log only the first time per point.
                if (!state.packageLost) {
                    state.packageLost = true;
                    SKSE::log::info("{} package replaced by {:08X} after {:.1f}s, reapplying",
                        Describe(a_actor), current ? current->GetFormID() : 0, state.sinceStep);
                }
                Apply(a_actor, state.intended);
            }

            if (!state.arrived && toTarget <= cfg.arrive_radius) {
                state.arrived = true;
                SKSE::log::info("{} arrived at #{} after {:.1f}s", Describe(a_actor), state.lastSeq, state.sinceStep);
            }
            if (!state.arrived && state.sinceStep < cfg.retarget_seconds) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

            // Next point: the freshest readable footprint ahead on the trail within sight
            const Trail::Footprint* next = nullptr;
            for (const auto* fp : readable) {
                if (fp->seq > state.lastSeq && pos.GetDistance(fp->pos) <= cfg.lead_distance &&
                    (!next || fp->seq > next->seq)) {
                    next = fp;
                }
            }
            // Nothing ahead in sight (e.g. the trail crossed stone): the first readable footprint
            // after the gap, measured from the last point, as a tracker casting around would find it
            bool gap = false;
            if (!next) {
                for (const auto* fp : readable) {
                    if (fp->seq > state.lastSeq && state.target.GetDistance(fp->pos) <= cfg.gap_distance &&
                        (!next || fp->seq < next->seq)) {
                        next = fp;
                        gap  = true;
                    }
                }
            }
            if (next) {
                if (gap) {
                    SKSE::log::info("{} picks the trail up again after a gap: #{} -> #{}",
                        Describe(a_actor), state.lastSeq, next->seq);
                }
                if (!state.arrived) {
                    SKSE::log::info("{} did not reach #{} in {:.1f}s (still {:.0f} away)",
                        Describe(a_actor), state.lastSeq, state.sinceStep, toTarget);
                }
                Steer(player, a_actor, state, *next);
            } else if (state.sinceStep >= cfg.lost_seconds) {
                Drop(a_actor, state, "lost the trail");
                seen.erase(id);
            } else if (state.intended != g_search) {
                // Arrived (or stuck for retarget_seconds) with nothing ahead yet
                Search(a_actor, state);
            }
            return RE::BSContainer::ForEachResult::kContinue;
        });

        // Trackers that left the High process (or were dropped above): undo and forget
        for (auto it = g_trackers.begin(); it != g_trackers.end();) {
            if (seen.contains(it->first)) {
                ++it;
                continue;
            }
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(it->first);
            if (actor && it->second.marker) {
                // Already released by Drop when it got here through "lost the trail"
                if (!it->second.marker->IsDeleted()) {
                    Drop(actor, it->second, "left the area");
                }
            }
            it = g_trackers.erase(it);
        }
    }

    void Clear() {
        g_trackers.clear();
        g_memory.clear();
    }

    std::vector<Binding> Bindings() {
        std::vector<Binding> result;
        for (const auto& [id, state] : g_trackers) {
            if (state.marker) {
                result.push_back({ id, state.marker->GetFormID(), state.prevLinked });
            }
        }
        return result;
    }

    void QueueStale(std::vector<Binding> a_bindings) {
        g_stale = std::move(a_bindings);
    }

    void ReleaseStale() {
        for (const auto& binding : g_stale) {
            auto* actor  = RE::TESForm::LookupByID<RE::Actor>(binding.actor);
            auto* marker = RE::TESForm::LookupByID<RE::TESObjectREFR>(binding.marker);
            Release(actor, marker, binding.prevLinked);
            SKSE::log::info("Released saved tracker binding: actor {:08X} marker {:08X} (found: {}, {})",
                binding.actor, binding.marker, actor != nullptr, marker != nullptr);
        }
        g_stale.clear();
    }
}
