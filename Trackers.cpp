#include "Trackers.h"

#include "Config.h"
#include "Telemetry.h"
#include "Trail.h"

#include <numbers>
#include <random>
#include <unordered_set>

namespace StealthSenses::Trackers {
    namespace {
        // Skyrim.esm forms, FormIDs read from the record headers of the local Skyrim.esm.
        // Both packages target PLDT type 6 (linked reference) with keyword LinkCustom02 (0005D5E7),
        // so one marker serves both:
        // defaultTravelToLinkCustom02 (template Travel 00016FAA) — walk to the marker;
        // defaultHoldPositionLinkCustom02_256UntilReleased (template 000503D0) — stay at it.
        constexpr RE::FormID kTravelPackage = 0x000235E2;
        constexpr RE::FormID kHoldPackage   = 0x0009D74A;
        constexpr RE::FormID kKeyword       = 0x0005D5E7;
        constexpr RE::FormID kXMarker       = 0x0000003B;
        constexpr RE::FormID kActorTypeNPC  = 0x00013794;

        // Animation graph events of humanoid idles (ENAM of the IDLE records in Skyrim.esm, all on
        // Actors\Character\Behaviors\0_Master.hkx). AIProcess::PlayIdle with the IDLE forms returned
        // false every time in test 0.4.0 (their conditions; IdleLookAround is even a hagraven idle),
        // so the events go straight to the graph.
        constexpr const char* kAnimExamine = "IdlePickup_Ground";  // bends to the ground
        constexpr const char* kAnimLook[]  = { "IdleLookFar", "IdleExamine" };

        RE::TESPackage*     g_travel   = nullptr;
        RE::TESPackage*     g_hold     = nullptr;
        RE::BGSKeyword*     g_keyword  = nullptr;
        RE::BGSKeyword*     g_humanoid = nullptr;
        RE::TESBoundObject* g_xmarker  = nullptr;
        bool                g_ready    = false;

        // Height difference beyond which a footprint or search point counts as another level
        constexpr float kMaxDz = 300.0f;
        // Movement below this per tick counts as standing still
        constexpr float kStillStep = 30.0f;

        std::mt19937 g_rng{ std::random_device{}() };

        // follow: walking to the next footprint; examine: standing at it, bent to the ground;
        // hop: walking to a search point around the last footprint; look: standing there, looking around
        enum class Mode { Follow, Examine, Hop, Look };

        constexpr const char* ModeName(Mode a_mode) {
            switch (a_mode) {
            case Mode::Follow:  return "follow";
            case Mode::Examine: return "examine";
            case Mode::Hop:     return "hop";
            default:            return "look";
            }
        }

        struct State {
            Mode                             mode      = Mode::Follow;
            std::uint32_t                    lastSeq   = 0;
            RE::NiPoint3                     trailPos;          // last footprint reached for
            RE::NiPoint3                     heading;           // trail direction at it, unit 2D
            RE::NiPoint3                     target;            // where the marker is now
            float                            sinceStep  = 0.0f; // since the last footprint was given
            float                            modeTime   = 0.0f;
            float                            examineCooldown = 0.0f;
            int                              hops       = 0;
            bool                             arrived    = false;
            bool                             alertSet   = false; // we put it into alert; undo on a calm drop
            RE::NiPoint3                     lastPos;           // NPC position last tick
            float                            stillTime  = 0.0f; // standing still while it should walk
            float                            stuckTotal = 0.0f; // all such time since the last footprint
            bool                             humanoid   = false;
            RE::NiPointer<RE::TESObjectREFR> marker;
            RE::FormID                       prevLinked  = 0;
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
        // Hostile NPCs that were in combat recently (seconds left): once the fight ends without
        // the player found, the NPC stays suspicious and takes up a trail it comes across
        std::unordered_map<RE::FormID, float>  g_recentCombat;
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

        // Log + telemetry; Say also echoes to the in-game console
        void Note(const std::string& a_text) {
            SKSE::log::info("{}", a_text);
            Telemetry::Log(a_text);
        }

        void Say(const std::string& a_text) {
            Note(a_text);
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

        const Trail::Footprint* FindBySeq(std::uint32_t a_seq) {
            // seq values are consecutive in the deque (Restore renumbers on load)
            const auto& trail = Trail::Footprints();
            if (trail.empty() || a_seq < trail.front().seq) {
                return nullptr;
            }
            const auto index = static_cast<std::size_t>(a_seq - trail.front().seq);
            return index < trail.size() && trail[index].seq == a_seq ? &trail[index] : nullptr;
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

        bool SetAlert(RE::Actor* a_actor, bool a_alert);

        void Drop(RE::Actor* a_actor, State& a_state, std::string_view a_why) {
            Say(std::format("{} {} after #{}, stops tracking", Describe(a_actor), a_why, a_state.lastSeq));
            Remember(a_actor->GetFormID(), a_state.lastSeq);
            Release(a_actor, a_state.marker.get(), a_state.prevLinked);
            // Gave up calmly: back to normal (vanilla may say an alert-to-normal line). In combat or
            // after spotting the player the alert belongs to vanilla now.
            if (a_state.alertSet && !a_actor->IsInCombat()) {
                SetAlert(a_actor, false);
            }
        }

        RE::TESPackage* PackageFor(Mode a_mode) {
            return a_mode == Mode::Follow || a_mode == Mode::Hop ? g_travel : g_hold;
        }

        // Arguments: temp package, not created by us (a vanilla form must not be freed),
        // allowed to pull the NPC out of furniture. Semantics are not verified [assumption].
        void Apply(RE::Actor* a_actor, RE::TESPackage* a_package) {
            a_actor->PutCreatedPackage(a_package, true, false, true);
        }

        void SetMode(RE::Actor* a_actor, State& a_state, Mode a_mode) {
            a_state.mode        = a_mode;
            a_state.modeTime    = 0.0f;
            a_state.arrived     = false;
            a_state.packageLost = false;
            a_state.stillTime   = 0.0f;
            Apply(a_actor, PackageFor(a_mode));
        }

        void PlayAnim(RE::Actor* a_actor, const State& a_state, const char* a_event) {
            if (!a_state.humanoid) {
                return;
            }
            const bool accepted = a_actor->NotifyAnimationGraph(a_event);
            Telemetry::Write({ { "type", "idle" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                { "event", a_event }, { "played", accepted } });
        }

        // Vanilla alert state (the lowProcessFlags kAlert bit, what Papyrus Actor.SetAlert sets).
        // Actor::SetIsAlerted — SE 36281 (0x1405D23C0, 1.5.97_comments.csv + offsets-1-5-97-0.csv),
        // AE 37270 (skyrimae.rename) [matched by name; not in se_ae.csv]. A 0x20-byte function
        // (IsAlerted follows right after), so a flag setter: void(Actor*, bool) [assumption].
        // Test 0.5.0: Papyrus SetAlert via VirtualMachine::DispatchMethodCall returned false — a
        // bandit has no bound script object to dispatch on. Returns whether the flag now matches.
        bool SetAlert(RE::Actor* a_actor, bool a_alert) {
            using func_t = void(RE::Actor*, bool);
            static REL::Relocation<func_t> setIsAlerted{ RELOCATION_ID(36281, 37270) };
            setIsAlerted(a_actor, a_alert);
            const auto* process = a_actor->GetActorRuntimeData().currentProcess;
            return process && process->lowProcessFlags.all(RE::AIProcess::LowProcessFlags::kAlert) == a_alert;
        }

        // Closest navmesh point, so search points do not land inside rocks and walls.
        // Pathing::GetPathingCell 29866/30682 + FindClosestPointOnNavmesh 29832/30648 [CL; no
        // open-source plugin calls them, result is checked by telemetry]
        std::optional<RE::NiPoint3> SnapToNavmesh(RE::Actor* a_actor, const RE::NiPoint3& a_point) {
            auto* pathing = RE::Pathing::GetSingleton();
            auto* cell    = a_actor->GetParentCell();
            if (!pathing || !cell) {
                return std::nullopt;
            }
            RE::BSTSmartPointer<RE::BSPathingCell> pathingCell;
            if (!pathing->GetPathingCell(a_point, cell, a_actor->GetWorldspace(), pathingCell) || !pathingCell) {
                return std::nullopt;
            }
            RE::NiPoint3 out;
            if (!pathing->FindClosestPointOnNavmesh(pathingCell, a_point, out)) {
                return std::nullopt;
            }
            return out;
        }

        void MoveMarker(State& a_state, const RE::NiPoint3& a_pos) {
            a_state.target = a_pos;
            a_state.marker->SetPosition(a_pos);
        }

        // Sends the NPC to the footprint: marker on the footprint, NPC linked to it, travel package.
        // False if the marker could not be placed; the caller must not keep the state then.
        bool Steer(RE::PlayerCharacter* a_player, RE::Actor* a_actor, State& a_state, const Trail::Footprint& a_fp) {
            const bool first = a_state.lastSeq == 0;
            if (!a_state.marker) {
                a_state.marker = a_player->PlaceObjectAtMe(g_xmarker, false);
                if (!a_state.marker) {
                    SKSE::log::error("{}: PlaceObjectAtMe(XMarker) failed", Describe(a_actor));
                    return false;
                }
                const auto* prev   = a_actor->extraList.GetLinkedRef(g_keyword);
                a_state.prevLinked = prev ? prev->GetFormID() : 0;
                a_actor->extraList.SetLinkedRef(a_state.marker.get(), g_keyword);
                a_state.humanoid = g_humanoid && a_actor->HasKeyword(g_humanoid);

                // A hunter with a weapon in hand reads as "tracking someone" at a glance
                const bool hostile = a_actor->IsHostileToActor(a_player);
                if (Config::Get().tracker.draw_weapon && a_state.humanoid && hostile) {
                    a_actor->DrawWeaponMagicHands(true);
                }
                if (Config::Get().tracker.set_alert && a_state.humanoid && hostile) {
                    a_state.alertSet = SetAlert(a_actor, true);
                    Telemetry::Write({ { "type", "alert" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                        { "flagSet", a_state.alertSet } });
                }
            }

            // Trail direction at this footprint, from the footprint before it
            if (const auto* prev = FindBySeq(a_fp.seq - 1)) {
                auto dir = a_fp.pos - prev->pos;
                dir.z    = 0.0f;
                if (const float len = dir.Length(); len > 1.0f) {
                    a_state.heading = dir / len;
                }
            }

            MoveMarker(a_state, a_fp.pos);
            a_state.lastSeq    = a_fp.seq;
            a_state.trailPos   = a_fp.pos;
            a_state.sinceStep  = 0.0f;
            a_state.stuckTotal = 0.0f;
            a_state.hops       = 0;
            a_state.lastPos    = a_actor->GetPosition();
            SetMode(a_actor, a_state, Mode::Follow);

            Say(std::format("{} {} footprint #{} dist {:.0f}", Describe(a_actor),
                first ? "picked up trail at" : "follows to", a_fp.seq, a_actor->GetPosition().GetDistance(a_fp.pos)));
            return true;
        }

        // The trail ran out: walk to points around the last footprint, biased along the trail
        // direction at first and wider with every hop, and look around at each.
        // Search points are at least this far from the NPC: travel packages stop ~270 short of the
        // marker, so a closer point meant two steps and "arrived" (test 0.4.0)
        constexpr float kMinHopWalk = 450.0f;

        void NextHop(RE::Actor* a_actor, State& a_state) {
            const auto& cfg = Config::Get().tracker;
            ++a_state.hops;
            const float spread  = std::min(50.0f * a_state.hops, 180.0f) * std::numbers::pi_v<float> / 180.0f;
            const float heading = a_state.heading.Length() > 0.5f ? std::atan2(a_state.heading.y, a_state.heading.x)
                                                                   : std::uniform_real_distribution<float>(-3.14159f, 3.14159f)(g_rng);
            const auto  npcPos  = a_actor->GetPosition();

            // A few random candidates; the first one on the navmesh, on this level and far enough
            // from the NPC wins, otherwise the farthest snapped one, otherwise the last raw one
            RE::NiPoint3 point;
            bool         snapped = false, good = false;
            float        bestWalk = -1.0f;
            for (int attempt = 0; attempt < 8 && !good; ++attempt) {
                const float angle = heading + std::uniform_real_distribution<float>(-spread, spread)(g_rng);
                const float dist  = std::uniform_real_distribution<float>(300.0f, std::max(cfg.search_radius, 400.0f))(g_rng);
                RE::NiPoint3 raw  = a_state.trailPos;
                raw.x += std::cos(angle) * dist;
                raw.y += std::sin(angle) * dist;

                const auto onMesh = SnapToNavmesh(a_actor, raw);
                const auto cand   = onMesh.value_or(raw);
                if (std::abs(cand.z - a_state.trailPos.z) > kMaxDz) {
                    continue;
                }
                const float walk = cand.GetDistance(npcPos);
                if (onMesh && walk > bestWalk) {
                    bestWalk = walk;
                    point    = cand;
                    snapped  = true;
                    good     = walk >= kMinHopWalk;
                } else if (!snapped) {
                    point = cand;
                }
            }
            if (point == RE::NiPoint3{}) {
                point = a_state.trailPos;
            }

            MoveMarker(a_state, point);
            SetMode(a_actor, a_state, Mode::Hop);
            Say(std::format("{} searches around #{}: hop {} ({:.0f} from it, {:.0f} to walk{})", Describe(a_actor),
                a_state.lastSeq, a_state.hops, point.GetDistance(a_state.trailPos), point.GetDistance(npcPos),
                snapped ? "" : ", not on navmesh"));
        }
    }

    bool Init() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            return false;
        }
        g_travel     = data->LookupForm<RE::TESPackage>(kTravelPackage, "Skyrim.esm");
        g_hold       = data->LookupForm<RE::TESPackage>(kHoldPackage, "Skyrim.esm");
        g_keyword    = data->LookupForm<RE::BGSKeyword>(kKeyword, "Skyrim.esm");
        g_humanoid   = data->LookupForm<RE::BGSKeyword>(kActorTypeNPC, "Skyrim.esm");
        // 0x3B is a hardcoded engine form below 0x800: LookupForm(…, "Skyrim.esm") returns
        // null for it (in-game test 0.2.0), the global lookup finds it
        if (auto* form = RE::TESForm::LookupByID(kXMarker)) {
            g_xmarker = form->As<RE::TESBoundObject>();
        }
        SKSE::log::info("Trackers: travel {:08X} hold {:08X} keyword {:08X} xmarker {:08X} humanoid {:08X}",
            g_travel ? g_travel->GetFormID() : 0, g_hold ? g_hold->GetFormID() : 0,
            g_keyword ? g_keyword->GetFormID() : 0, g_xmarker ? g_xmarker->GetFormID() : 0,
            g_humanoid ? g_humanoid->GetFormID() : 0);
        // The humanoid keyword is cosmetic: without it no animations play
        g_ready = g_travel && g_hold && g_keyword && g_xmarker;
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

        // Footprints worth a look at all: clear ones (>= min_visibility) are read from afar, faint
        // ones (>= faint_visibility, e.g. stone at 0.15) only within close_read_radius of the NPC —
        // in a stone dungeon nothing was readable and trackers only searched (test 0.4.1)
        struct Candidate {
            const Trail::Footprint* fp;
            bool                    clear;
        };
        std::vector<Candidate> candidates;
        std::size_t            clearCount = 0;
        for (const auto& fp : Trail::Footprints()) {
            if (fp.space != space) {
                continue;
            }
            const float vis = Trail::Visibility(fp, now, badWeather);
            if (vis >= cfg.faint_visibility) {
                const bool clear = vis >= cfg.min_visibility;
                candidates.push_back({ &fp, clear });
                clearCount += clear;
            }
        }
        auto readableFrom = [&](const Candidate& a_c, const RE::NiPoint3& a_npc) {
            return a_c.clear || a_npc.GetDistance(a_c.fp->pos) <= cfg.close_read_radius;
        };

        for (auto& [id, memory] : g_memory) {
            memory.cooldown = std::max(memory.cooldown - a_deltaSeconds, 0.0f);
        }
        std::erase_if(g_recentCombat, [&](auto& a_entry) { return (a_entry.second -= a_deltaSeconds) <= 0.0f; });

        if (Telemetry::Enabled()) {
            Telemetry::Write({ { "type", "player" }, { "pos", Telemetry::Vec(player->GetPosition()) },
                { "space", Telemetry::Hex(space) }, { "sneak", player->IsSneaking() },
                { "run", player->IsRunning() }, { "combat", player->IsInCombat() },
                { "weather", badWeather }, { "readable", clearCount }, { "faint", candidates.size() - clearCount } });
        }

        std::unordered_set<RE::FormID> seen;

        processes->ForEachHighActor([&](RE::Actor* a_actor) {
            if (!IsCandidate(a_actor, player, space)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const auto id      = a_actor->GetFormID();
            const auto tracked = g_trackers.find(id);

            const bool combat  = a_actor->IsInCombat();
            const bool hostile = a_actor->IsHostileToActor(player);
            const int  meter   = StealthMeter(a_actor, player);
            const bool alerted = a_actor->GetActorRuntimeData().currentProcess->lowProcessFlags.all(
                RE::AIProcess::LowProcessFlags::kAlert);

            if (Telemetry::Enabled()) {
                // State before this tick's decision; the decision itself goes out as a "log" record
                const auto*    package = a_actor->GetCurrentPackage();
                const auto*    state   = a_actor->AsActorState();
                nlohmann::json npc{ { "type", "npc" }, { "id", Telemetry::Hex(id) }, { "name", a_actor->GetName() },
                    { "pos", Telemetry::Vec(a_actor->GetPosition()) }, { "meter", meter }, { "alert", alerted },
                    { "combat", combat }, { "hostile", hostile }, { "afterFight", g_recentCombat.contains(id) },
                    { "sitsleep", state ? static_cast<int>(state->GetSitSleepState()) : -1 },
                    { "weapon", state && state->IsWeaponDrawn() },
                    { "pkg", Telemetry::Hex(package ? package->GetFormID() : 0) } };
                if (tracked != g_trackers.end()) {
                    const auto& s = tracked->second;
                    npc["track"] = { { "seq", s.lastSeq }, { "target", Telemetry::Vec(s.target) },
                        { "trail", Telemetry::Vec(s.trailPos) }, { "mode", ModeName(s.mode) },
                        { "since", s.sinceStep }, { "modeTime", s.modeTime }, { "hops", s.hops } };
                }
                Telemetry::Write(std::move(npc));
            }

            if (combat && hostile && a_actor->GetActorRuntimeData().currentCombatTarget.get().get() == player) {
                g_recentCombat[id] = cfg.after_combat_seconds;
            }
            if (combat && !cfg.include_combat) {
                if (tracked != g_trackers.end()) {
                    Drop(a_actor, tracked->second, "entered combat");
                    g_trackers.erase(tracked);
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (cfg.require_hostile && !hostile) {
                return RE::BSContainer::ForEachResult::kContinue;
            }

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
                // Why it would look for a trail at all. The stealth meter alone is a poor signal: any
                // hostile within ~2000 units sits at 41+ and within 500 at 59-67 with nothing going on
                // (tests 0.5.x), so caution_level is set above that; losing the player in a fight is
                // the strong trigger
                const bool afterFight = g_recentCombat.contains(id);
                const bool suspicious = cfg.debug_all_npcs || alerted || afterFight || meter >= cfg.caution_level;
                if (!suspicious || g_trackers.size() >= static_cast<std::size_t>(cfg.max_trackers)) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto memory = g_memory.find(id);
                if (memory != g_memory.end() && memory->second.cooldown > 0.0f) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }
                const auto skipSeq = memory != g_memory.end() ? memory->second.lastSeq : 0;

                // Sitting, sleeping or on the way into/out of furniture: not looking at the ground
                const auto* actorState = a_actor->AsActorState();
                const auto  sitSleep   = actorState ? actorState->GetSitSleepState() : RE::SIT_SLEEP_STATE::kNormal;
                const bool  idleBody   = sitSleep != RE::SIT_SLEEP_STATE::kNormal;

                // Notices only a footprint right next to it and in front of it (GetHeadingAngle:
                // degrees between where the NPC faces and the point [CL]); takes the freshest one
                const Trail::Footprint* best   = nullptr;
                int                     nearCount = 0;
                int                     viewCount = 0;
                for (const auto& c : candidates) {
                    const auto* fp = c.fp;
                    if (fp->seq <= skipSeq || pos.GetDistance(fp->pos) > cfg.notice_radius || !readableFrom(c, pos)) {
                        continue;
                    }
                    ++nearCount;
                    if (a_actor->GetHeadingAngle(fp->pos, true) > cfg.notice_fov * 0.5f) {
                        continue;
                    }
                    ++viewCount;
                    if (!idleBody && (!best || fp->seq > best->seq)) {
                        best = fp;
                    }
                }
                if (nearCount > 0) {
                    // Why a footprint next to an NPC was or was not noticed
                    Telemetry::Write({ { "type", "notice" }, { "id", Telemetry::Hex(id) }, { "near", nearCount },
                        { "inView", viewCount }, { "sitsleep", static_cast<int>(sitSleep) }, { "picked", best != nullptr } });
                }
                if (best) {
                    Note(std::format("{} noticed footprint #{} (meter {}, alert {}, after fight {})", Describe(a_actor), best->seq, meter, alerted, afterFight));
                    State state;
                    if (Steer(player, a_actor, state, *best)) {
                        g_trackers.emplace(id, std::move(state));
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

            auto& state = tracked->second;
            state.sinceStep += a_deltaSeconds;
            state.modeTime += a_deltaSeconds;
            state.examineCooldown = std::max(state.examineCooldown - a_deltaSeconds, 0.0f);

            const auto* current = a_actor->GetCurrentPackage();
            if (current != PackageFor(state.mode)) {
                // Another package won the evaluation (seen in test 0.2.1: dynamic FF… packages
                // and the NPC's own patrol). Put ours back; log only the first time per mode.
                if (!state.packageLost) {
                    state.packageLost = true;
                    Note(std::format("{} package replaced by {:08X} in {} after {:.1f}s, reapplying",
                        Describe(a_actor), current ? current->GetFormID() : 0, ModeName(state.mode), state.modeTime));
                }
                Apply(a_actor, PackageFor(state.mode));
            }

            const float toTarget = pos.GetDistance(state.target);
            if (!state.arrived && toTarget <= cfg.arrive_radius) {
                state.arrived = true;
                if (state.mode == Mode::Follow) {
                    Note(std::format("{} arrived at #{} after {:.1f}s", Describe(a_actor), state.lastSeq, state.modeTime));
                }
            }

            // Standing still while it should be walking: the point is unreachable (another level,
            // behind a wall) — test 0.4.0 had a tracker frozen for 40 s while targets kept coming
            const bool  walking = state.mode == Mode::Follow || state.mode == Mode::Hop;
            const float moved   = pos.GetDistance(state.lastPos);
            state.lastPos       = pos;
            if (walking && !state.arrived && moved < kStillStep) {
                state.stillTime += a_deltaSeconds;
                state.stuckTotal += a_deltaSeconds;
            } else {
                state.stillTime = 0.0f;
            }
            if (state.stuckTotal >= cfg.stuck_give_up) {
                Drop(a_actor, state, std::format("got stuck ({:.0f}s without moving)", state.stuckTotal));
                seen.erase(id);
                return RE::BSContainer::ForEachResult::kContinue;
            }
            const bool stuck = walking && state.stillTime >= cfg.stuck_seconds;

            // Continuation of the trail: the freshest readable footprint ahead within sight, or
            // after a gap (e.g. the trail crossed stone) the first one near the last footprint.
            // Footprints on another level are skipped: the straight-line distance lies there.
            const Trail::Footprint* next = nullptr;
            for (const auto& c : candidates) {
                const auto* fp = c.fp;
                if (fp->seq > state.lastSeq && pos.GetDistance(fp->pos) <= cfg.lead_distance && readableFrom(c, pos) &&
                    std::abs(fp->pos.z - pos.z) <= kMaxDz && (!next || fp->seq > next->seq)) {
                    next = fp;
                }
            }
            // Across a gap only clear footprints: a faint one is not seen from a distance
            bool gap = false;
            if (!next) {
                for (const auto& c : candidates) {
                    const auto* fp = c.fp;
                    if (c.clear && fp->seq > state.lastSeq && state.trailPos.GetDistance(fp->pos) <= cfg.gap_distance &&
                        std::abs(fp->pos.z - state.trailPos.z) <= kMaxDz && (!next || fp->seq < next->seq)) {
                        next = fp;
                        gap  = true;
                    }
                }
            }
            auto follow = [&]() {
                if (gap) {
                    Note(std::format("{} picks the trail up again after a gap: #{} -> #{}", Describe(a_actor), state.lastSeq, next->seq));
                }
                Steer(player, a_actor, state, *next);
            };
            auto searchOrGiveUp = [&]() {
                if (state.sinceStep >= cfg.lost_seconds) {
                    Drop(a_actor, state, "lost the trail");
                    seen.erase(id);
                } else if (state.hops == 0 && state.mode != Mode::Look) {
                    // First stop and look around where the trail ends: at the head of a fresh
                    // trail the next footprint usually appears within seconds, and hopping off at
                    // once made the tracker dart back and forth every second (test 0.4.1)
                    SetMode(a_actor, state, Mode::Look);
                    PlayAnim(a_actor, state, kAnimLook[0]);
                    Say(std::format("{} stops at #{} and looks around", Describe(a_actor), state.lastSeq));
                } else {
                    NextHop(a_actor, state);
                }
            };

            switch (state.mode) {
            case Mode::Follow:
                if (stuck) {
                    // Can't get to this footprint: search from here instead of taking the next one,
                    // which is usually behind the same obstacle
                    Note(std::format("{} can't reach #{} ({:.0f} away, not moving {:.0f}s), searches instead",
                        Describe(a_actor), state.lastSeq, toTarget, state.stillTime));
                    searchOrGiveUp();
                } else if (state.arrived && state.humanoid && state.examineCooldown <= 0.0f) {
                    // Bends down to read the footprint before going on
                    state.examineCooldown = cfg.examine_every;
                    SetMode(a_actor, state, Mode::Examine);
                    PlayAnim(a_actor, state, kAnimExamine);
                    Say(std::format("{} examines footprint #{}", Describe(a_actor), state.lastSeq));
                } else if (state.arrived || state.modeTime >= cfg.retarget_seconds) {
                    if (!state.arrived) {
                        Note(std::format("{} did not reach #{} in {:.1f}s (still {:.0f} away)",
                            Describe(a_actor), state.lastSeq, state.modeTime, toTarget));
                    }
                    if (next) {
                        follow();
                    } else {
                        searchOrGiveUp();
                    }
                }
                break;
            case Mode::Examine:
                if (state.modeTime >= cfg.examine_seconds) {
                    if (next) {
                        follow();
                    } else {
                        searchOrGiveUp();
                    }
                }
                break;
            case Mode::Hop:
                if (next) {
                    Say(std::format("{} found the trail again at #{}", Describe(a_actor), next->seq));
                    follow();
                } else if (state.sinceStep >= cfg.lost_seconds) {
                    searchOrGiveUp();
                } else if (stuck) {
                    Note(std::format("{} can't reach search point {} ({:.0f} away), next one",
                        Describe(a_actor), state.hops, toTarget));
                    NextHop(a_actor, state);
                } else if (state.arrived || state.modeTime >= cfg.hop_timeout) {
                    SetMode(a_actor, state, Mode::Look);
                    PlayAnim(a_actor, state, kAnimLook[state.hops % std::size(kAnimLook)]);
                }
                break;
            case Mode::Look:
                if (next) {
                    Say(std::format("{} found the trail again at #{}", Describe(a_actor), next->seq));
                    follow();
                } else if (state.modeTime >= cfg.look_seconds) {
                    searchOrGiveUp();
                }
                break;
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
        g_recentCombat.clear();
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
