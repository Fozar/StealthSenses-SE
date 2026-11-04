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

#include "Trackers.h"

#include "Config.h"
#include "Detection.h"
#include "Profiles.h"
#include "Overlay.h"
#include "ScentLogic.h"
#include "Telemetry.h"
#include "Trail.h"
#include "TrailLogic.h"

#include <numbers>
#include <random>
#include <set>
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

        // A footprint or search point this much higher or lower than the NPC is on another level (a
        // ledge, a cliff, the floor above): not noticed, not followed (tracker.max_height_diff)
        bool OtherLevel(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to) {
            return std::abs(a_to.z - a_from.z) > Config::Get().tracker.max_height_diff;
        }
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
            bool                             scent = false;
            std::uint32_t                    scentSegment = 0;
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
            float                            stuckTotal = 0.0f; // all such time since it last reached a point
            bool                             humanoid   = false;
            RE::NiPointer<RE::TESObjectREFR> marker;
            RE::FormID                       prevLinked  = 0;
            bool                             packageLost = false;
            float                            total      = 0.0f; // since it started tracking: gets tired
            RE::NiPoint3                     startPos;          // where it started: leash
            float                            fight      = 0.0f; // > 0: fighting someone else, trail on hold
            std::string                      profile;           // its sense profile, for the dev overlay
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
        // NPCs that found the body of one of their own killed by the player (seconds left): suspicious
        std::unordered_map<RE::FormID, float>  g_foundBody;
        // Bodies of the player's victims (from TESDeathEvent) with the game hour of death, until
        // older than body_max_age_hours, disabled or deleted. Saved in the co-save.
        std::vector<KnownBody>                 g_bodies;
        // (NPC, corpse) pairs: each body is found once per NPC. Saved too, or a loaded game would
        // send every NPC to bodies it already searched around.
        std::set<std::pair<RE::FormID, RE::FormID>> g_bodiesSeen;
        std::vector<Binding>                   g_stale;
        bool                                   g_sightTested = false;

        // RequestDetectionLevel returns < 0 while the target is undetected; MaxsuDetectionMeter
        // maps it to a 0..100 meter as level + 100, and any value >= 0 to 100 (detected).
        // repos/max-su-2019_MaxsuDetectionMeter/src/DataHandler.cpp:21-28 [prior art, not RE]
        int StealthMeter(std::int32_t a_level) {
            return a_level < 0 ? std::clamp(a_level + 100, 0, 100) : 100;
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

        // The player, someone it commands (a summon) or its follower
        bool IsPlayerSide(RE::Actor* a_actor, RE::PlayerCharacter* a_player) {
            return a_actor && (a_actor == a_player || a_actor->GetCommandingActor().get() == a_player ||
                                  a_actor->IsPlayerTeammate());
        }

        // One of its own: the corpse is in a faction the NPC is in too (rank >= 0) — bandit and
        // bandit, guard and a citizen of the same hold. A dead mudcrab is nobody's own.
        // [assumption: good enough, tune by test]
        bool IsOwn(RE::Actor* a_actor, RE::Actor* a_corpse) {
            return a_corpse->VisitFactions([&](RE::TESFaction* a_faction, std::int8_t a_rank) {
                return a_faction && a_rank >= 0 && a_actor->IsInFaction(a_faction);
            });
        }

        void Remember(RE::FormID a_id, std::uint32_t a_lastSeq, float a_cooldown) {
            auto& memory    = g_memory[a_id];
            memory.lastSeq  = std::max(memory.lastSeq, a_lastSeq);
            memory.cooldown = a_cooldown;
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

        // a_cooldown: seconds before it may take up a trail again; repickup_seconds when omitted
        void Drop(RE::Actor* a_actor, State& a_state, std::string_view a_why, std::optional<float> a_cooldown = {}) {
            Say(std::format("{} {} after #{}, stops tracking", Describe(a_actor), a_why, a_state.lastSeq));
            Remember(a_actor->GetFormID(), a_state.lastSeq, a_cooldown.value_or(Config::Get().tracker.repickup_seconds));
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

        // Vanilla alert state: the kAlert bit of AIProcess::lowProcessFlags [CL], written directly.
        // That is all the engine setter does — disassembly of runtime 1.7.104: Actor::SetIsAlerted
        // (AE 37270) is "currentProcess ? ActorProcess::SetIsAlerted : return", and
        // ActorProcess::SetIsAlerted (AE 39165) sets or clears bit 3 of the byte at +0x136 and
        // returns. Up to 0.7.1 the plugin called 37270 by ID, whose SE pair (36281) was only matched
        // by name; the field needs no ID on any runtime. Test 0.5.0: Papyrus SetAlert via
        // DispatchMethodCall returned false — a bandit has no bound script object to dispatch on.
        bool SetAlert(RE::Actor* a_actor, bool a_alert) {
            auto* process = a_actor->GetActorRuntimeData().currentProcess;
            if (!process) {
                return false;
            }
            process->lowProcessFlags.set(a_alert, RE::AIProcess::LowProcessFlags::kAlert);
            return true;
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

        // Line of sight from the NPC's eyes to a footprint: without it a footprint behind a wall
        // or a rock was noticed if it was close and in front (known limitation of 0.7.0).
        // bhkWorld::PickObject (virtual 33) + bhkPickData [CL]; the ray setup — the actor's own
        // filter so its body is skipped, layer kLOS, BS_TO_HK scale — is the one GrabAndThrow
        // uses (repos/powerof3_GrabAndThrow/src/TrajectoryOverlay.cpp:49-64) [prior art].
        // Actors in the way do not hide a footprint. The ray ends a little above the ground, and
        // a hit within kLosSlack of that end counts as seen: at a shallow angle the ground right
        // around the footprint gets in the way first [assumption].
        constexpr float kEyeFactor      = 0.9f;  // eyes at 90% of the actor's height
        constexpr float kLosAboveGround = 16.0f;
        constexpr float kLosSlack       = 48.0f;
        constexpr int   kMaxSightRays   = 6;  // per NPC and tick, when it notices footprints

        enum class Sight { Seen, Blocked, Unknown };

        Sight CanSee(RE::Actor* a_actor, const RE::NiPoint3& a_point, bool a_scent = false) {
            auto* cell  = a_actor->GetParentCell();
            auto* world = cell ? cell->GetbhkWorld() : nullptr;
            if (!world) {
                return Sight::Unknown;
            }
            auto eye = a_actor->GetPosition();
            eye.z += a_actor->GetHeight() * kEyeFactor;
            auto to = a_point;
            to.z += kLosAboveGround;

            RE::CFilter filter;
            a_actor->GetCollisionFilterInfo(filter);
            filter.SetCollisionLayer(RE::COL_LAYER::kLOS);

            RE::bhkPickData pick;
            pick.rayInput.from                        = eye * RE::bhkWorld::GetWorldScale();
            pick.rayInput.to                          = to * RE::bhkWorld::GetWorldScale();
            pick.rayInput.filterInfo                  = filter;
            pick.rayInput.enableShapeCollectionFilter = true;
            world->PickObject(pick);

            const auto* collidable = pick.rayOutput.rootCollidable;
            const auto  layer      = collidable ? collidable->GetCollisionLayer() : RE::COL_LAYER::kUnidentified;
            const float length     = eye.GetDistance(to);
            const float shortBy    = collidable ? length * (1.0f - pick.rayOutput.hitFraction) : 0.0f;
            auto        result     = Sight::Seen;
            switch (layer) {
            case RE::COL_LAYER::kUnidentified:  // no hit
            case RE::COL_LAYER::kCharController:
            case RE::COL_LAYER::kBiped:
            case RE::COL_LAYER::kBipedNoCC:
            case RE::COL_LAYER::kDeadBip:
                break;
            default:
                result = shortBy <= kLosSlack ? Sight::Seen : Sight::Blocked;
                break;
            }
            // Scent uses this ray only as a conservative barrier check, independent of FOV and
            // the visual LOS setting. No end slack or actor passthrough: a wall next to the source
            // must not become transparent. This does not simulate diffusion around obstacles.
            if (a_scent && collidable) result = shortBy <= 0.0f ? Sight::Seen : Sight::Blocked;
            // Every ray: what it hit and how far short of the footprint (whether walls block at all)
            Telemetry::Write({ { "type", "sight" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                { "eye", Telemetry::Vec(eye) }, { "to", Telemetry::Vec(to) }, { "hit", collidable != nullptr },
                { "layer", static_cast<int>(layer) }, { "short", shortBy }, { "blocked", result == Sight::Blocked },
                { "channel", a_scent ? "scent" : "visual" } });
            return result;
        }

        // Does the NPC see the body of one of its own the player killed? Our own perception, like
        // footprints: within body_notice_radius, in the view cone, line of sight to it.
        struct Body {
            RE::Actor*   actor;
            RE::NiPoint3 pos;
        };

        RE::Actor* SeesOwnBody(RE::Actor* a_actor, const std::vector<Body>& a_bodies) {
            const auto& cfg = Config::Get().tracker;
            const auto  pos = a_actor->GetPosition();
            for (const auto& body : a_bodies) {
                if (pos.GetDistance(body.pos) > cfg.body_notice_radius ||
                    a_actor->GetHeadingAngle(body.pos, true) > cfg.notice_fov * 0.5f ||
                    g_bodiesSeen.contains({ a_actor->GetFormID(), body.actor->GetFormID() }) || !IsOwn(a_actor, body.actor)) {
                    continue;
                }
                // A blocked look is in the "sight" record of the ray; "body" only when found
                if (CanSee(a_actor, body.pos) != Sight::Blocked) {
                    Telemetry::Write({ { "type", "body" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                        { "corpse", Telemetry::Hex(body.actor->GetFormID()) }, { "dist", pos.GetDistance(body.pos) },
                        { "via", "sight" } });
                    g_bodiesSeen.emplace(a_actor->GetFormID(), body.actor->GetFormID());
                    return body.actor;
                }
            }
            return nullptr;
        }

        // Does the NPC already know of such a body? MiddleHighProcessData::deadDetectList [CL] holds
        // the dead the NPC learned of: test 0.8 — the body was in it from the moment of the kill
        // (heard, before any sight of it) to the end of the run, and it stayed empty for a bandit
        // that never noticed the death. Checked when vanilla's own search is over (combat NPCs are
        // not looked at), so the NPC goes on to the body instead of returning to its post.
        // Only fresh victims we remember (a_bodies: killed by the player within body_max_age_hours)
        // and within body_know_radius — no walk across the whole dungeon to a body it heard of.
        RE::Actor* KnowsOwnBody(RE::Actor* a_actor, const std::vector<Body>& a_bodies) {
            const auto* process = a_actor->GetActorRuntimeData().currentProcess;
            if (!process || !process->middleHigh) {
                return nullptr;
            }
            const auto& cfg = Config::Get().tracker;
            for (const auto& handle : process->middleHigh->deadDetectList) {
                const auto corpse = handle.get();
                if (!corpse || g_bodiesSeen.contains({ a_actor->GetFormID(), corpse->GetFormID() })) {
                    continue;
                }
                const auto body = std::ranges::find(a_bodies, corpse.get(), &Body::actor);
                if (body == a_bodies.end() || a_actor->GetPosition().GetDistance(body->pos) > cfg.body_know_radius ||
                    !IsOwn(a_actor, corpse.get())) {
                    continue;
                }
                g_bodiesSeen.emplace(a_actor->GetFormID(), corpse->GetFormID());
                Telemetry::Write({ { "type", "body" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                    { "corpse", Telemetry::Hex(corpse->GetFormID()) }, { "dist", a_actor->GetPosition().GetDistance(body->pos) },
                    { "via", "deadList" } });
                return corpse.get();
            }
            return nullptr;
        }

        void MoveMarker(State& a_state, const RE::NiPoint3& a_pos) {
            a_state.target = a_pos;
            a_state.marker->SetPosition(a_pos);
        }

        // Takes the NPC over the first time: marker placed, NPC linked to it, weapon drawn, alert.
        // False if the marker could not be placed; the caller must not keep the state then.
        bool Attach(RE::PlayerCharacter* a_player, RE::Actor* a_actor, State& a_state) {
            if (a_state.marker) {
                return true;
            }
            a_state.marker = a_player->PlaceObjectAtMe(g_xmarker, false);
            if (!a_state.marker) {
                SKSE::log::error("{}: PlaceObjectAtMe(XMarker) failed", Describe(a_actor));
                return false;
            }
            a_state.startPos   = a_actor->GetPosition();
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
            return true;
        }

        // Sends the NPC to the footprint: marker on the footprint, travel package.
        bool Steer(RE::PlayerCharacter* a_player, RE::Actor* a_actor, State& a_state, const Trail::Footprint& a_fp) {
            const bool first = a_state.lastSeq == 0;
            if (!Attach(a_player, a_actor, a_state)) {
                return false;
            }

            // Trail direction at this footprint, from the footprint before it
            if (const auto* prev = Trail::FindBySeq(a_fp.seq - 1); prev && (!a_state.scent || Scent::Connected(*prev, a_fp))) {
                auto dir = a_fp.pos - prev->pos;
                dir.z    = 0.0f;
                if (const float len = dir.Length(); len > 1.0f) {
                    a_state.heading = dir / len;
                }
            }

            MoveMarker(a_state, a_fp.pos);
            a_state.lastSeq    = a_fp.seq;
            a_state.scentSegment = a_state.scent ? a_fp.scentSegment : 0;
            a_state.trailPos   = a_fp.pos;
            a_state.sinceStep  = 0.0f;
            if (first) {
                a_state.stuckTotal = 0.0f;
            }
            // Otherwise kept: a frozen tracker that picks the next footprint each time it gives up on
            // one never got dropped (test 0.9 dev: a minute of follow -> stuck -> look -> follow on a
            // line of footprints it could not walk to). Reaching a point clears it, walking pays it back.
            a_state.hops       = 0;
            a_state.lastPos    = a_actor->GetPosition();
            SetMode(a_actor, a_state, Mode::Follow);

            Say(std::format("{} {} footprint #{} dist {:.0f} via {} segment {}", Describe(a_actor),
                first ? "picked up trail at" : "follows to", a_fp.seq, a_actor->GetPosition().GetDistance(a_fp.pos),
                a_state.scent ? "scent" : "visual", a_state.scentSegment));
            Telemetry::Write({ { "type", "track_target" }, { "id", Telemetry::Hex(a_actor->GetFormID()) },
                { "seq", a_fp.seq }, { "channel", a_state.scent ? "scent" : "visual" }, { "segment", a_state.scentSegment } });
            return true;
        }

        // Found the body of one of its own: walks over to it as to a search point. On the way and
        // there the usual search runs — a footprint close by is taken up (lastSeq 0: any of the
        // player's), otherwise it looks around and searches points around the body, and gives up
        // after lost_seconds.
        bool Investigate(RE::PlayerCharacter* a_player, RE::Actor* a_actor, State& a_state, const RE::NiPoint3& a_pos) {
            if (!Attach(a_player, a_actor, a_state)) {
                return false;
            }
            MoveMarker(a_state, a_pos);
            a_state.trailPos   = a_pos;
            a_state.heading    = {};
            a_state.sinceStep  = 0.0f;
            a_state.stuckTotal = 0.0f;
            a_state.hops       = 0;
            a_state.lastPos    = a_actor->GetPosition();
            SetMode(a_actor, a_state, Mode::Hop);
            Say(std::format("{} goes to the body, dist {:.0f}", Describe(a_actor), a_actor->GetPosition().GetDistance(a_pos)));
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
                if (OtherLevel(a_state.trailPos, cand)) {
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

    namespace {
        // 0.9: what Detection adds to this NPC's detection level on the player (0 = not suspicious)
        std::int32_t DetectionBonus(RE::FormID a_id) {
            const auto& cfg = Config::Get().detection;
            if (!cfg.enabled) {
                return 0;
            }
            // In combat detection stays vanilla: the vanilla combat search ("Who's there?") sits at
            // level 0, on the edge, and +10 turned it into a sure find right after an arrow from
            // hiding (release smoke test 0.9). after_fight_bonus is for after the fight.
            if (const auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_id); !actor || actor->IsInCombat()) {
                return 0;
            }
            int bonus = 0;
            if (const auto it = g_trackers.find(a_id); it != g_trackers.end() && !it->second.scent) {
                bonus = std::max(bonus, cfg.tracker_bonus);
            }
            if (g_recentCombat.contains(a_id)) {
                bonus = std::max(bonus, cfg.after_fight_bonus);
            }
            if (g_foundBody.contains(a_id)) {
                bonus = std::max(bonus, cfg.found_body_bonus);
            }
            return bonus;
        }

        void PublishDetectionBonuses() {
            std::unordered_set<RE::FormID> suspects;
            for (const auto& [id, state] : g_trackers) {
                suspects.insert(id);
            }
            for (const auto& [id, left] : g_recentCombat) {
                suspects.insert(id);
            }
            for (const auto& [id, left] : g_foundBody) {
                suspects.insert(id);
            }
            std::vector<std::pair<RE::FormID, std::int32_t>> bonuses;
            for (const auto id : suspects) {
                if (const auto bonus = DetectionBonus(id); bonus > 0) {
                    bonuses.emplace_back(id, bonus);
                }
            }
            Detection::PublishBonuses(std::move(bonuses));
        }
    }

    void Update(float a_deltaSeconds) {
        // However the tick ends, the detection hook gets the suspects as they are now
        struct PublishOnExit {
            ~PublishOnExit() { PublishDetectionBonuses(); }
        } publishOnExit;

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
        const float scentWeather = Scent::WeatherFactor(exterior, sky && sky->IsRaining(), sky && sky->IsSnowing(), Config::Get().scent);

        // Once per loaded game: a ray from the player's eyes into the ground under its feet must be
        // blocked. If it is not, the ray setup sees no world geometry and the line-of-sight check
        // passes everything (test 0.7.2: only unobstructed rays, nothing to tell the two apart).
        if (!g_sightTested && cfg.notice_line_of_sight) {
            g_sightTested = true;
            auto below    = player->GetPosition();
            below.z -= 200.0f;
            const auto sight = CanSee(player, below);
            SKSE::log::info("Sight self-test: ray into the ground under the player is {} (expected blocked)",
                sight == Sight::Blocked ? "blocked" : sight == Sight::Seen ? "NOT blocked" : "unknown, no havok world");
        }

        // The ticker already pruned points unused by either visual tracking or scent.

        // 0.11: darkness by the hour outside, constant inside; profiles without night vision lose by it
        const auto& dark     = Config::Get().dark;
        const float darkness = Trail::Darkness(calendar->GetHour(), exterior, dark);

        // The player's victims lying here now; gone and old ones are forgotten (an old body is no
        // news, and the footprints around it have faded anyway)
        std::vector<Body> bodies;
        std::erase_if(g_bodies, [&](const KnownBody& a_body) {
            auto* corpse = RE::TESForm::LookupByID<RE::Actor>(a_body.corpse);
            if (!corpse || corpse->IsDeleted() || corpse->IsDisabled() || !corpse->IsDead() ||
                now - a_body.diedAt > cfg.body_max_age_hours) {
                std::erase_if(g_bodiesSeen, [&](const BodySeen& a_seen) { return a_seen.second == a_body.corpse; });
                return true;
            }
            if (Trail::SpaceOf(corpse) == space && corpse->Is3DLoaded()) {
                bodies.push_back({ corpse, corpse->GetPosition() });
            }
            return false;
        });

        // Footprints worth a look at all, with the trail's own visibility; each NPC reads them through
        // its profile (Reading below). The counts are for the telemetry: as a plain human reads them.
        struct Candidate {
            const Trail::Footprint* fp;
            float                   vis;
            Scent::Sample           scent;
        };
        std::vector<Candidate> candidates;
        std::size_t            clearCount = 0;
        std::size_t            faintCount = 0;
        {
            const Profiles::Profile human;
            for (const auto& fp : Trail::Footprints()) {
                if (fp.space != space) {
                    continue;
                }
                const float vis = Trail::Visibility(fp, now, badWeather);
                const auto scent = Scent::SampleAt(fp, now, Config::Get().scent);
                // Use the hard visual bound: layered tracking_mult rules can exceed the largest
                // single rule's boost. Scent-only sources survive this prefilter as well.
                if (vis * Profiles::kMaxTracking * Profiles::kMaxBlood >= cfg.faint_visibility ||
                    scent.peak >= Config::Get().scent.min_strength) {
                    candidates.push_back({ &fp, vis, scent });
                    const float read = Profiles::Perceived(vis, fp.flags & Trail::kBlood, darkness, dark.night_penalty, human);
                    clearCount += read >= cfg.min_visibility;
                    faintCount += read >= cfg.faint_visibility && read < cfg.min_visibility;
                }
            }
        }
        // How one NPC reads them: clear ones (>= min_visibility) from afar, faint ones (>=
        // faint_visibility, e.g. stone at 0.15) only within close_read_radius — in a stone dungeon
        // nothing was readable and trackers only searched (test 0.4.1)
        struct Reading {
            const Profiles::Profile& profile;
            float                    darkness;
            float                    nightPenalty;
            const Config::TrackerSettings& cfg;

            float Perceived(const Candidate& a_c) const {
                return Profiles::Perceived(a_c.vis, a_c.fp->flags & Trail::kBlood, darkness, nightPenalty, profile);
            }
            bool Clear(const Candidate& a_c) const { return profile.tracking > 0 && Perceived(a_c) >= cfg.min_visibility; }
            bool ReadableFrom(const Candidate& a_c, const RE::NiPoint3& a_at) const {
                if (profile.tracking <= 0) return false;
                const float vis = Perceived(a_c);
                return vis >= cfg.min_visibility ||
                       (vis >= cfg.faint_visibility && a_at.GetDistance(a_c.fp->pos) <= cfg.close_read_radius);
            }
        };

        // A memory is kept while its cooldown runs or its last footprint is still in the trail;
        // after that it means nothing (every footprint left is newer) — without this the map grew
        // with every NPC that ever tracked, until the next load
        const auto& trail    = Trail::Footprints();
        const auto  oldest   = trail.empty() ? std::numeric_limits<std::uint32_t>::max() : trail.front().seq;
        std::erase_if(g_memory, [&](auto& a_entry) {
            auto& memory    = a_entry.second;
            memory.cooldown = std::max(memory.cooldown - a_deltaSeconds, 0.0f);
            return memory.cooldown <= 0.0f && memory.lastSeq < oldest;
        });
        std::erase_if(g_recentCombat, [&](auto& a_entry) { return (a_entry.second -= a_deltaSeconds) <= 0.0f; });
        std::erase_if(g_foundBody, [&](auto& a_entry) { return (a_entry.second -= a_deltaSeconds) <= 0.0f; });

        if (Telemetry::Enabled()) {
            Telemetry::Write({ { "type", "player" }, { "pos", Telemetry::Vec(player->GetPosition()) },
                { "space", Telemetry::Hex(space) }, { "sneak", player->IsSneaking() },
                { "run", player->IsRunning() }, { "combat", player->IsInCombat() },
                { "weather", badWeather }, { "scentWeather", scentWeather }, { "dark", darkness }, { "readable", clearCount }, { "faint", faintCount } });
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
            const auto level   = a_actor->RequestDetectionLevel(player);
            const int  meter   = StealthMeter(level);
            const bool alerted = a_actor->GetActorRuntimeData().currentProcess->lowProcessFlags.all(
                RE::AIProcess::LowProcessFlags::kAlert);
            const auto    profile = Profiles::For(a_actor);
            const Reading reading{ profile, darkness, dark.night_penalty, cfg };

            if (Telemetry::Enabled()) {
                // State before this tick's decision; the decision itself goes out as a "log" record
                const auto*    package = a_actor->GetCurrentPackage();
                const auto*    state   = a_actor->AsActorState();
                nlohmann::json npc{ { "type", "npc" }, { "id", Telemetry::Hex(id) }, { "name", a_actor->GetName() },
                    { "pos", Telemetry::Vec(a_actor->GetPosition()) }, { "meter", meter }, { "level", level }, { "bonus", DetectionBonus(id) }, { "alert", alerted },
                    { "combat", combat }, { "hostile", hostile }, { "afterFight", g_recentCombat.contains(id) }, { "foundBody", g_foundBody.contains(id) },
                    { "sitsleep", state ? static_cast<int>(state->GetSitSleepState()) : -1 },
                    { "weapon", state && state->IsWeaponDrawn() },
                    { "pkg", Telemetry::Hex(package ? package->GetFormID() : 0) },
                    { "prof", { { "name", profile.name }, { "tracking", profile.tracking }, { "night", profile.night }, { "blood", profile.blood },
                        { "smell", profile.smell }, { "smellBlood", profile.smellBlood }, { "smellHunt", profile.smellHunt } } } };
                // Vanilla "dead bodies this NPC detected" list, to learn what fills it
                if (const auto* mh = a_actor->GetActorRuntimeData().currentProcess->middleHigh; mh && !mh->deadDetectList.empty()) {
                    npc["deadList"] = mh->deadDetectList.size();
                }
                // Dev probe: raw CalculateDetection outputs next to the meter (open_questions #1)
                if (const auto dv = Detection::LastOnPlayer(id)) {
                    npc["dv"] = { { "v", dv->value }, { "u4", dv->unk04 }, { "u5", dv->unk05 }, { "u6", dv->unk06 },
                        { "pos", Telemetry::Vec(dv->pos) }, { "f", { dv->unk08, dv->unk09, dv->unk10 } }, { "age", dv->ageMs } };
                }
                if (tracked != g_trackers.end()) {
                    const auto& s = tracked->second;
                    npc["track"] = { { "seq", s.lastSeq }, { "target", Telemetry::Vec(s.target) },
                        { "trail", Telemetry::Vec(s.trailPos) }, { "mode", ModeName(s.mode) },
                        { "since", s.sinceStep }, { "modeTime", s.modeTime }, { "hops", s.hops }, { "fight", s.fight },
                        { "channel", s.scent ? "scent" : "visual" }, { "segment", s.scentSegment } };
                }
                Telemetry::Write(std::move(npc));
            }

            if (combat && hostile && a_actor->GetActorRuntimeData().currentCombatTarget.get().get() == player) {
                g_recentCombat[id] = cfg.after_combat_seconds;
            }
            if (combat && !cfg.include_combat) {
                if (tracked != g_trackers.end()) {
                    // A fight with someone else (a wolf on the road, a guard) only interrupts the
                    // chase: the marker and the link stay (combat AI ignores packages), its clocks
                    // stand still, and it goes back to the trail once the fight is over. A fight
                    // with the player's side, or with nobody known (vanilla's "who's there"
                    // search is combat too), is vanilla's business.
                    auto*       target = a_actor->GetActorRuntimeData().currentCombatTarget.get().get();
                    auto&       state  = tracked->second;
                    const bool  other  = target && !IsPlayerSide(target, player);
                    if (other && state.fight + a_deltaSeconds <= cfg.fight_pause_seconds) {
                        if (state.fight == 0.0f) {
                            Say(std::format("{} fights {}, keeps the trail at #{}", Describe(a_actor), Describe(target), state.lastSeq));
                        }
                        state.fight += std::max(a_deltaSeconds, 0.001f);
                        seen.insert(id);
                    } else {
                        Drop(a_actor, state, other ? std::format("fought {} too long", Describe(target)) : "entered combat");
                        g_trackers.erase(tracked);
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (cfg.require_hostile && !hostile) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            // Reading footprints takes a mind: animals, automatons and the undead have tracking 0 in
            // the profiles (before 0.11 humanoids_only). Animals see and hear far better (meter 86-98
            // in test 0.6.0) and count as hostile: rabbits and deer tailed the player.
            if (profile.tracking <= 0.0f && (!Config::Get().scent.enabled || profile.smell <= 0.0f)) {
                return RE::BSContainer::ForEachResult::kContinue;
            }
            std::optional<RE::NiPoint3> bodyPos;
            if (profile.tracking > 0 && cfg.body_found_seconds > 0.0f) {
                auto* corpse = SeesOwnBody(a_actor, bodies);
                const bool saw = corpse != nullptr;
                if (!corpse) {
                    corpse = KnowsOwnBody(a_actor, bodies);
                }
                if (corpse) {
                    g_foundBody[id] = cfg.body_found_seconds;
                    bodyPos         = corpse->GetPosition();
                    Say(std::format("{} {} the body of {}", Describe(a_actor), saw ? "found" : "knows of", Describe(corpse)));
                }
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

            // Freshest locally perceived source, bounded rays per NPC/tick. Never sense from the
            // travel marker: vanilla stops ~270 units before it, which is not the NPC's nose.
            auto scentNext = [&](std::uint32_t a_skip, std::uint32_t a_segment, bool a_suspicious, bool a_pickup) {
                const auto& sc = Config::Get().scent;
                const Trail::Footprint* best = nullptr;
                int nearby = 0, rays = 0, blocked = 0, unknown = 0;
                float strongest = 0, pickedStrength = 0;
                Scent::Sample pickedSource;
                std::vector<const Trail::Footprint*> inRange;
                if (sc.enabled && profile.smell > 0) {
                    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                        const auto& c = *it;
                        const auto* fp = c.fp;
                        if (!Scent::Ahead(*fp, a_skip, a_segment) || OtherLevel(pos, fp->pos)) continue;
                        const float strength = Scent::Perceived(c.scent, pos.GetDistance(fp->pos), fp->flags & Trail::kBlood, profile, sc, scentWeather);
                        strongest = std::max(strongest, strength);
                        if (strength < sc.min_strength || strength <= 0) continue;
                        ++nearby;
                        if (a_pickup && !a_suspicious && !profile.smellHunt && !(cfg.alarm_blood && (fp->flags & Trail::kBlood))) continue;
                        inRange.push_back(fp);
                    }
                    best = Scent::FirstReachable(inRange, [&](const Trail::Footprint& fp) {
                        ++rays;
                        const auto sight = CanSee(a_actor, fp.pos, true);
                        if (sight != Sight::Seen) {
                            blocked += sight == Sight::Blocked;
                            unknown += sight == Sight::Unknown;
                            return false;
                        }
                        return true;
                    }, kMaxSightRays);
                    if (best) {
                        pickedSource = Scent::SampleAt(*best, now, sc);
                        pickedStrength = Scent::Perceived(pickedSource, pos.GetDistance(best->pos), best->flags & Trail::kBlood, profile, sc, scentWeather);
                    }
                }
                if (nearby || a_segment) {
                    Telemetry::Write({ { "type", "scent_notice" }, { "id", Telemetry::Hex(id) },
                        { "stage", a_pickup ? "pickup" : "continue" }, { "segment", a_segment },
                        { "near", nearby }, { "rays", rays }, { "blocked", blocked }, { "unknown", unknown },
                        { "strongest", strongest }, { "perceived", pickedStrength }, { "source", pickedSource.peak }, { "scentWeather", scentWeather },
                        { "spreadRadius", pickedSource.radius },
                        { "picked", best ? best->seq : 0 }, { "pickedSegment", best ? best->scentSegment : 0 },
                        { "reason", best ? "picked" : !sc.enabled ? "disabled" : profile.smell <= 0 ? "no_sense" :
                            rays ? "barrier_or_budget" : nearby ? "calm" : "no_local_source" } });
                }
                return best;
            };

            if (tracked == g_trackers.end() && bodyPos && g_trackers.size() < static_cast<std::size_t>(cfg.max_trackers)) {
                // Just found a body: goes to it and searches from there
                State state;
                if (Investigate(player, a_actor, state, *bodyPos)) {
                    g_trackers.emplace(id, std::move(state));
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (tracked == g_trackers.end()) {
                // Why it would look for a trail at all. The stealth meter alone is a poor signal: any
                // hostile within ~2000 units sits at 41+ and within 500 at 59-67 with nothing going on
                // (tests 0.5.x), so caution_level is set above that; losing the player in a fight is
                // the strong trigger, and so is finding the body of one of its own the player killed
                const bool afterFight = g_recentCombat.contains(id);
                const bool foundBody  = g_foundBody.contains(id);
                const bool suspicious = cfg.debug_all_npcs || alerted || afterFight || foundBody || meter >= cfg.caution_level;
                // 0.11: a calm one still takes up an alarming footprint — a werewolf's or a vampire
                // lord's, or a bloody one — if it sees it
                const bool anyAlarm = cfg.alarm_beast || cfg.alarm_blood;
                if ((!suspicious && !anyAlarm && !profile.smellHunt) || g_trackers.size() >= static_cast<std::size_t>(cfg.max_trackers)) {
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
                // degrees between where the NPC faces and the point [CL]) and not hidden from its
                // eyes; takes the freshest such one. The radius grows with the profile's tracking (and
                // blood for a bloody footprint). Not suspicious: only an alarming footprint will do.
                auto alarming = [&](const Trail::Footprint* a_fp) {
                    return (cfg.alarm_beast && (a_fp->flags & (Trail::kBeast | Trail::kVampireLord))) ||
                           (cfg.alarm_blood && (a_fp->flags & Trail::kBlood));
                };
                std::vector<const Trail::Footprint*> inView;
                int                                  nearCount = 0;
                for (const auto& c : candidates) {
                    const auto* fp     = c.fp;
                    const float radius = Profiles::NoticeRadius(cfg.notice_radius, fp->flags & Trail::kBlood, profile);
                    if (fp->seq <= skipSeq || (!suspicious && !alarming(fp)) || pos.GetDistance(fp->pos) > radius ||
                        OtherLevel(pos, fp->pos) || !reading.ReadableFrom(c, pos)) {
                        continue;
                    }
                    ++nearCount;
                    if (a_actor->GetHeadingAngle(fp->pos, true) <= cfg.notice_fov * 0.5f) {
                        inView.push_back(fp);
                    }
                }
                // Freshest first; a few rays at most per NPC and tick (candidates are in seq order)
                const Trail::Footprint* best    = nullptr;
                int                     blocked = 0;
                // Out of rays with every one blocked: the rest is behind the same wall
                for (auto it = inView.rbegin(); !idleBody && it != inView.rend() && blocked < kMaxSightRays; ++it) {
                    if (!cfg.notice_line_of_sight || CanSee(a_actor, (*it)->pos) != Sight::Blocked) {
                        best = *it;
                        break;
                    }
                    ++blocked;
                }
                if (nearCount > 0) {
                    // Why a footprint next to an NPC was or was not noticed
                    Telemetry::Write({ { "type", "notice" }, { "id", Telemetry::Hex(id) }, { "near", nearCount },
                        { "inView", inView.size() }, { "blocked", blocked }, { "sitsleep", static_cast<int>(sitSleep) },
                        { "picked", best != nullptr } });
                }
                bool viaScent = false;
                if (!best && !idleBody) {
                    best = scentNext(skipSeq, 0, suspicious, true);
                    viaScent = best != nullptr;
                }
                if (best) {
                    const char* alarm = (best->flags & Trail::kBeast)       ? ", a werewolf's" :
                                        (best->flags & Trail::kVampireLord) ? ", a vampire lord's" :
                                        (best->flags & Trail::kBlood)       ? ", bloody" : "";
                    (suspicious ? Note : Say)(std::format("{} noticed footprint #{}{} (meter {}, alert {}, after fight {}, found body {}, profile {})",
                        Describe(a_actor), best->seq, alarm, meter, alerted, afterFight, foundBody, profile.name));
                    State state;
                    state.scent = viaScent;
                    state.profile = profile.name;
                    if (Steer(player, a_actor, state, *best)) {
                        g_trackers.emplace(id, std::move(state));
                    }
                }
                return RE::BSContainer::ForEachResult::kContinue;
            }

            auto& state   = tracked->second;
            state.profile = profile.name;
            if (state.scent && (!Config::Get().scent.enabled || profile.smell <= 0)) {
                Drop(a_actor, state, "no longer senses scent");
                seen.erase(id);
                return RE::BSContainer::ForEachResult::kContinue;
            }
            if (state.fight > 0.0f) {
                // The fight is over and the player is not in sight: back to where it was. The fight
                // dragged it off and vanilla may have dropped the alert; the mode starts over.
                Say(std::format("{} is done fighting ({:.0f}s), back to the trail at #{}", Describe(a_actor), state.fight, state.lastSeq));
                state.fight      = 0.0f;
                state.lastPos    = pos;
                state.stuckTotal = 0.0f;
                if (state.alertSet) {
                    SetAlert(a_actor, true);
                }
                SetMode(a_actor, state, state.mode);
            }
            state.sinceStep += a_deltaSeconds;
            state.modeTime += a_deltaSeconds;
            state.total += a_deltaSeconds;
            state.examineCooldown = std::max(state.examineCooldown - a_deltaSeconds, 0.0f);

            // Enough is enough: a long chase however fresh the trail, or too far from where it
            // started (bandits do not leave their camp for the horizon). Without these a tracker
            // fed with new footprints followed for good. The longer cooldown keeps it from taking
            // the same chase up again a moment later.
            const float fromStart = pos.GetDistance(state.startPos);
            const bool  tired     = state.total >= cfg.give_up_seconds;
            if (tired || fromStart >= cfg.leash_distance) {
                Drop(a_actor, state,
                    tired ? std::format("got tired ({:.0f}s on the trail)", state.total)
                          : std::format("went too far ({:.0f} from where it started)", fromStart),
                    cfg.tired_cooldown);
                seen.erase(id);
                return RE::BSContainer::ForEachResult::kContinue;
            }

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
                state.stuckTotal = 0.0f;
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
                if (walking && moved >= kStillStep) {
                    // Walking pays the stuck time back: a long chase with short stalls is not stuck,
                    // a tracker frozen in a loop of footprints is
                    state.stuckTotal = std::max(state.stuckTotal - a_deltaSeconds, 0.0f);
                }
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
            if (state.scent) {
                next = scentNext(state.lastSeq, state.scentSegment, true, false);
            }
            for (const auto& c : candidates) {
                if (state.scent) break;
                const auto* fp = c.fp;
                // A faint footprint is also read around the footprint the NPC was sent to: travel
                // stops ~270 short of it, so measured from the NPC alone the next faint one was
                // always just out of reach and the tracker stopped at every step (fox, test 0.6.0)
                const bool readable = reading.ReadableFrom(c, pos) || reading.ReadableFrom(c, state.trailPos);
                if (fp->seq > state.lastSeq && pos.GetDistance(fp->pos) <= cfg.lead_distance && readable &&
                    !OtherLevel(pos, fp->pos) && (!next || fp->seq > next->seq)) {
                    next = fp;
                }
            }
            // Across a gap only clear footprints: a faint one is not seen from a distance. Following a
            // trail, the continuation is the first footprint after the gap; with no trail yet
            // (lastSeq 0: started from a body) the freshest one — the first was the player's oldest
            // print near the body, 2468 units off (test 0.8)
            bool       gap      = false;
            const bool freshest = state.lastSeq == 0;
            if (!next && !state.scent) {
                for (const auto& c : candidates) {
                    const auto* fp = c.fp;
                    if (reading.Clear(c) && fp->seq > state.lastSeq && state.trailPos.GetDistance(fp->pos) <= cfg.gap_distance &&
                        !OtherLevel(state.trailPos, fp->pos) && !OtherLevel(pos, fp->pos) &&
                        (!next || (freshest ? fp->seq > next->seq : fp->seq < next->seq))) {
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
                } else if (state.arrived && state.humanoid && !state.scent && state.examineCooldown <= 0.0f) {
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
            auto* actor  = RE::TESForm::LookupByID<RE::Actor>(it->first);
            auto& marker = it->second.marker;
            // Already released by Drop when it got here through "lost the trail"
            if (marker && !marker->IsDeleted()) {
                if (actor) {
                    // Skipped this tick for a reason of its own: a wolf killed a tracker mid-fight
                    // (test 0.8.1), or a debug-key tracker is not hostile
                    const char* why = actor->IsDead()                                               ? "died" :
                                      cfg.require_hostile && player && !actor->IsHostileToActor(player) ? "is not hostile" :
                                                                                                          "left the area";
                    Drop(actor, it->second, why);
                } else {
                    // The actor is gone (deleted): the marker must not stay in the world and the save
                    Release(nullptr, marker.get(), 0);
                }
            }
            it = g_trackers.erase(it);
        }
    }

    void NoteKill(RE::FormID a_corpse, RE::FormID a_killer) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* corpse = RE::TESForm::LookupByID<RE::Actor>(a_corpse);
        auto* killer = RE::TESForm::LookupByID<RE::Actor>(a_killer);
        auto* calendar = RE::Calendar::GetSingleton();
        if (!corpse || !calendar || !IsPlayerSide(killer, player) ||
            std::ranges::contains(g_bodies, a_corpse, &KnownBody::corpse)) {
            return;
        }
        g_bodies.push_back({ a_corpse, calendar->GetHoursPassed() });
        Note(std::format("{} killed by {}, body remembered", Describe(corpse), Describe(killer)));
    }

    void DebugTrack(RE::Actor* a_actor) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!g_ready || !a_actor || !player || a_actor == player || a_actor->IsDead()) {
            return;
        }
        // Visual mode bypasses notice conditions. Scent mode still needs a local, reachable source;
        // try a bounded sequence of nearest candidates, just as natural pickup checks alternatives.
        const auto              space = Trail::SpaceOf(a_actor);
        const auto              pos   = a_actor->GetPosition();
        const auto profile = Profiles::For(a_actor);
        const bool scent = Overlay::ScentOnly() || (profile.tracking <= 0 && profile.smell > 0);
        const auto* calendar = RE::Calendar::GetSingleton();
        const auto* cell = a_actor->GetParentCell();
        const auto* sky = RE::Sky::GetSingleton();
        const float scentWeather = Scent::WeatherFactor(cell && !cell->IsInteriorCell(), sky && sky->IsRaining(), sky && sky->IsSnowing(), Config::Get().scent);
        std::vector<const Trail::Footprint*> nearby;
        for (const auto& fp : Trail::Footprints()) {
            if (scent && (!calendar ||
                Scent::Perceived(Scent::SampleAt(fp, calendar->GetHoursPassed(), Config::Get().scent), pos.GetDistance(fp.pos),
                    fp.flags & Trail::kBlood, profile, Config::Get().scent, scentWeather) < Config::Get().scent.min_strength ||
                fp.scentSegment == 0)) continue;
            if (fp.space == space && !OtherLevel(pos, fp.pos)) nearby.push_back(&fp);
        }
        std::ranges::stable_sort(nearby, {}, [&](const auto* fp) { return pos.GetDistance(fp->pos); });
        const auto* best = scent ? Scent::FirstReachable(nearby, [&](const Trail::Footprint& fp) {
            return CanSee(a_actor, fp.pos, true) == Sight::Seen;
        }, kMaxSightRays) : nearby.empty() ? nullptr : nearby.front();
        if (!best) {
            Say(std::format("debug: no {} source in {}'s space/level/range or within barrier ray budget", scent ? "scent" : "visual", Describe(a_actor)));
            return;
        }
        auto& state = g_trackers[a_actor->GetFormID()];
        state.scent = scent;
        state.profile = profile.name;
        if (!Steer(player, a_actor, state, *best)) {
            g_trackers.erase(a_actor->GetFormID());
            return;
        }
        if (Config::Get().tracker.require_hostile && !a_actor->IsHostileToActor(player)) {
            Say(std::format("debug: {} is not hostile, require_hostile will drop it next tick", Describe(a_actor)));
        }
    }

    void DebugFindBody(RE::Actor* a_actor) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!g_ready || !a_actor || !player || a_actor == player || a_actor->IsDead()) {
            return;
        }
        const auto pos  = a_actor->GetPosition();
        RE::Actor* body = nullptr;
        for (const auto& known : g_bodies) {
            auto* corpse = RE::TESForm::LookupByID<RE::Actor>(known.corpse);
            if (corpse && corpse->Is3DLoaded() && Trail::SpaceOf(corpse) == Trail::SpaceOf(a_actor) &&
                (!body || pos.GetDistance(corpse->GetPosition()) < pos.GetDistance(body->GetPosition()))) {
                body = corpse;
            }
        }
        if (!body) {
            Say("debug: no body of the player's victims here (console: <ref>.kill player)");
            return;
        }
        const auto id = a_actor->GetFormID();
        g_bodiesSeen.emplace(id, body->GetFormID());
        g_foundBody[id] = Config::Get().tracker.body_found_seconds;
        Say(std::format("debug: {} knows of the body of {}", Describe(a_actor), Describe(body)));
        auto& state = g_trackers[id];
        if (!Investigate(player, a_actor, state, body->GetPosition())) {
            g_trackers.erase(id);
        }
    }

    void DebugPaintTrail(RE::Actor* a_origin) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        // In front of the given NPC (to test what it does with a trail under its nose), else the player
        RE::Actor* from = a_origin ? a_origin : player;
        if (!player || !from) {
            return;
        }
        const auto  space = Trail::SpaceOf(from);
        const float angle = from->GetAngleZ();  // 0 = north (+y), clockwise
        const RE::NiPoint3 dir{ std::sin(angle), std::cos(angle), 0.0f };
        auto        origin = from->GetPosition();
        int         count  = 0;
        Trail::BreakScent("paint start");
        // Every 64 units like real steps, from 2 m ahead to ~30 m; each point dropped onto the
        // navmesh so it lies on walkable ground
        for (float d = 128.0f; d <= 2100.0f; d += 64.0f) {
            const auto point = SnapToNavmesh(player, origin + dir * d).value_or(origin + dir * d);
            Trail::Inject(point, space, RE::MATERIAL_ID::kDirt, "painted");
            ++count;
        }
        Say(std::format("debug: painted {} footprints ahead of {}", count, Describe(from)));
        Trail::BreakScent("paint end");
    }

    void DebugReset() {
        for (auto& [id, state] : g_trackers) {
            if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id); actor && state.marker && !state.marker->IsDeleted()) {
                Drop(actor, state, "debug reset", 0.0f);
            }
        }
        g_trackers.clear();
        g_memory.clear();
        g_recentCombat.clear();
        g_foundBody.clear();
        g_bodiesSeen.clear();
        PublishDetectionBonuses();
        Say(std::format("debug: reset — trackers released, memories and body reactions forgotten ({} bodies kept)", g_bodies.size()));
    }

    std::vector<KnownBody> Bodies() {
        return g_bodies;
    }

    std::vector<BodySeen> BodiesSeen() {
        return { g_bodiesSeen.begin(), g_bodiesSeen.end() };
    }

    void RestoreBodies(std::vector<KnownBody> a_bodies, std::vector<BodySeen> a_seen) {
        g_bodies = std::move(a_bodies);
        g_bodiesSeen.clear();
        g_bodiesSeen.insert(a_seen.begin(), a_seen.end());
        SKSE::log::info("Trackers: restored {} bodies, {} reactions to them", g_bodies.size(), g_bodiesSeen.size());
    }

    void Clear() {
        g_trackers.clear();
        g_memory.clear();
        g_recentCombat.clear();
        g_foundBody.clear();
        g_bodiesSeen.clear();
        g_bodies.clear();
        g_sightTested = false;
        PublishDetectionBonuses();
    }

    std::vector<DebugView> DebugViews() {
        std::vector<DebugView> result;
        for (const auto& [id, state] : g_trackers) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (!actor || !actor->Is3DLoaded()) {
                continue;
            }
            const char* name   = actor->GetName();
            const bool  search = state.mode == Mode::Hop || state.mode == Mode::Look;
            auto        label  = std::format("{} {} {} #{} {:.0f}s seg{}", name && *name ? name : "?",
                        state.scent ? "SCENT" : "VISUAL", ModeName(state.mode), state.lastSeq, state.modeTime, state.scentSegment);
            if (search) {
                label += std::format(" hop {}", state.hops);
            }
            if (state.fight > 0.0f) {
                label += " fighting";
            }
            if (!state.profile.empty()) {
                label += " [" + state.profile + "]";
            }
            result.push_back({ actor->GetPosition(), state.target, state.trailPos, search, std::move(label) });
        }
        return result;
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
            // The alert flag we set is saved with the game: without this a reloaded tracker was
            // alerted, hence suspicious, and took the trail up again 0.1 s after load (test 0.6.1)
            if (actor && Config::Get().tracker.set_alert && !actor->IsInCombat()) {
                SetAlert(actor, false);
            }
            SKSE::log::info("Released saved tracker binding: actor {:08X} marker {:08X} (found: {}, {})",
                binding.actor, binding.marker, actor != nullptr, marker != nullptr);
        }
        g_stale.clear();
    }
}
