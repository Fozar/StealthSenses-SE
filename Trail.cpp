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

#include "Trail.h"

#include "Config.h"
#include "Telemetry.h"
#include "TrailLogic.h"

namespace StealthSenses::Trail {
    namespace {
        std::deque<Footprint> g_trail;
        std::uint32_t         g_nextSeq    = 1;
        int                   g_bloodSteps = 0;

        using Clock = std::chrono::steady_clock;
        Clock::time_point g_lastFootstepEvent{};

        // [esm] Skyrim.esm / Dawnguard.esm record headers
        constexpr RE::FormID kLightFootPerk     = 0x05820C;  // LightFoot
        constexpr RE::FormID kWerewolfRace      = 0x0CDD84;  // WerewolfBeastRace
        constexpr RE::FormID kVampireLordRace   = 0x00283A;  // DLC1VampireBeastRace, Dawnguard.esm

        // Resolved on kDataLoaded; read on the task threads after that
        RE::BGSPerk*                                        g_lightFoot = nullptr;
        const RE::TESRace*                                  g_werewolf  = nullptr;
        const RE::TESRace*                                  g_vampireLord = nullptr;
        std::unordered_map<const RE::TESRace*, RaceKind>    g_raceKinds;

        void AddRaces(RE::TESDataHandler* a_data, const std::vector<std::string>& a_refs, RaceKind a_kind, std::string_view a_name) {
            for (const auto& text : a_refs) {
                const auto ref  = Config::ParseFormRef(text);
                const auto* race = ref ? a_data->LookupForm<RE::TESRace>(ref->second, ref->first) : nullptr;
                if (!race) {
                    // Not an error: a race mod from the list may not be installed
                    SKSE::log::info("Trail: {} race '{}' not found", a_name, text);
                    continue;
                }
                g_raceKinds[race] = a_kind;
            }
        }

        // Footstep events stop in the air, the poll sampler does not (DEVELOPMENT §6: footprints
        // in the air after a fall). [CL] bhkCharacterController::context.currentState
        bool OnGround(RE::PlayerCharacter* a_player) {
            const auto* controller = a_player->GetCharController();
            return !controller || controller->context.currentState == RE::hkpCharacterStateType::kOnGround;
        }

        // Dawnguard's vampire lord behavior graph has "isLevitating" [esm: name in Dawnguard.esm
        // conditions]; type not checked — int first, then bool
        bool Levitating(RE::PlayerCharacter* a_player) {
            std::int32_t asInt = 0;
            if (a_player->GetGraphVariableInt("isLevitating", asInt)) {
                return asInt != 0;
            }
            bool asBool = false;
            return a_player->GetGraphVariableBool("isLevitating", asBool) && asBool;
        }

        bool WearsHeavy(RE::PlayerCharacter* a_player, RE::BGSBipedObjectForm::BipedObjectSlot a_slot) {
            const auto* armor = a_player->GetWornArmor(a_slot);
            return armor && armor->IsHeavyArmor();
        }

        Stride ReadStride(RE::PlayerCharacter* a_player, RE::MATERIAL_ID a_material, bool a_interior) {
            Stride s;
            if (const auto* av = a_player->AsActorValueOwner()) {
                s.sneakSkill = av->GetActorValue(RE::ActorValue::kSneak);
            }
            // Sneaking first: sneak-running is still sneaking
            const auto* state = a_player->AsActorState();
            if (a_player->IsSneaking()) {
                s.gait = Gait::kSneak;
            } else if (state && state->IsSprinting()) {
                s.gait = Gait::kSprint;
            } else if (state && state->IsWalking()) {
                s.gait = Gait::kWalk;
            }
            using Slot       = RE::BGSBipedObjectForm::BipedObjectSlot;
            s.heavyBoots     = WearsHeavy(a_player, Slot::kFeet);
            s.heavyBody      = WearsHeavy(a_player, Slot::kBody);
            s.overEncumbered = a_player->IsOverEncumbered();
            s.lightFoot      = g_lightFoot && a_player->HasPerk(g_lightFoot);
            const auto* race = a_player->GetRace();
            s.beastForm      = race && (race == g_werewolf || race == g_vampireLord);
            if (const auto it = g_raceKinds.find(race); it != g_raceKinds.end()) {
                s.race = it->second;
            }
            s.interior = a_interior;
            s.material = a_material;
            return s;
        }

        constexpr std::string_view GaitName(Gait a_gait) {
            switch (a_gait) {
            case Gait::kWalk:
                return "walk";
            case Gait::kSprint:
                return "sprint";
            case Gait::kSneak:
                return "sneak";
            default:
                return "run";
            }
        }

        constexpr std::string_view RaceName(RaceKind a_race) {
            switch (a_race) {
            case RaceKind::kKhajiit:
                return "khajiit";
            case RaceKind::kArgonian:
                return "argonian";
            case RaceKind::kBosmer:
                return "bosmer";
            default:
                return "other";
            }
        }

        RE::MATERIAL_ID SurfaceUnder(RE::PlayerCharacter* a_player) {
            // [engine_api §6] bhkCharacterController::surfaceMaterial, works indoors too
            auto material = RE::MATERIAL_ID::kNone;
            if (const auto* controller = a_player->GetCharController()) {
                material = controller->surfaceMaterial;
            }
            // On exterior landscape the controller mostly reports kNone (2nd in-game test:
            // 384 of 547 footprints). Fall back to the land texture material, as DTD does:
            // repos/maglarnet_DTD/src/SurfaceProfiles.cpp:658, TES::GetLandMaterialType 13203/13349
            if (material == RE::MATERIAL_ID::kNone) {
                const auto* cell = a_player->GetParentCell();
                const auto* tes  = RE::TES::GetSingleton();
                if (cell && !cell->IsInteriorCell() && tes) {
                    material = tes->GetLandMaterialType(a_player->GetPosition());
                }
            }
            return material;
        }
    }

    RE::FormID SpaceOf(const RE::TESObjectREFR* a_ref) {
        if (!a_ref) {
            return 0;
        }
        const auto* cell = a_ref->GetParentCell();
        if (!cell) {
            return 0;
        }
        if (cell->IsInteriorCell()) {
            return cell->GetFormID();
        }
        const auto* world = a_ref->GetWorldspace();
        return world ? world->GetFormID() : 0;
    }

    void TryRecord(RE::PlayerCharacter* a_player, std::string_view a_source) {
        const auto& cfg = Config::Get().trail;
        if (!cfg.enabled || !a_player) {
            return;
        }

        const auto space = SpaceOf(a_player);
        if (space == 0) {
            return;
        }

        // Water breaks the trail; mounts are out of scope for the prototype
        if (const auto* state = a_player->AsActorState(); state && state->IsSwimming()) {
            return;
        }
        if (a_player->IsOnMount()) {
            return;
        }
        // Jumping, falling, levitating: no ground to leave a print in
        if (!OnGround(a_player)) {
            return;
        }
        if (g_vampireLord && a_player->GetRace() == g_vampireLord && Levitating(a_player)) {
            return;
        }

        const auto pos = a_player->GetPosition();
        if (!g_trail.empty()) {
            const auto& last = g_trail.back();
            if (last.space == space && last.pos.GetDistance(pos) < cfg.min_spacing) {
                return;
            }
        }

        const auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar) {
            return;
        }

        // Wading through shallow water (not swimming) washes footprints away too
        const auto material = SurfaceUnder(a_player);
        if (material == RE::MATERIAL_ID::kWater) {
            return;
        }

        Footprint fp;
        fp.pos       = pos;
        fp.space     = space;
        fp.gameHours = calendar->GetHoursPassed();
        fp.material  = material;
        fp.seq       = g_nextSeq++;
        const auto* cell   = a_player->GetParentCell();
        const auto  stride = ReadStride(a_player, material, cell && cell->IsInteriorCell());
        fp.step            = StepMult(stride, Config::Get().step);
        if (g_bloodSteps > 0) {
            fp.flags |= kBlood;
            --g_bloodSteps;
        }
        // A beast form on the ground: alarming to anyone who sees it (0.11); levitating left none
        if (stride.beastForm) {
            fp.flags |= a_player->GetRace() == g_werewolf ? kBeast : kVampireLord;
        }

        g_trail.push_back(fp);
        if (g_trail.size() > static_cast<std::size_t>(cfg.max_footprints)) {
            const auto before = g_trail.size();
            Thin(g_trail, cfg);
            SKSE::log::debug("Trail: thinned {} -> {} footprints", before, g_trail.size());
        }

        Telemetry::Write({ { "type", "footprint" }, { "seq", fp.seq }, { "pos", Telemetry::Vec(pos) },
            { "space", Telemetry::Hex(space) }, { "mat", Telemetry::Hex(static_cast<std::uint32_t>(fp.material)) },
            { "base", Visibility(fp, fp.gameHours, false) }, { "blood", (fp.flags & kBlood) != 0 },
            { "beast", (fp.flags & kBeast) != 0 }, { "vlord", (fp.flags & kVampireLord) != 0 },
            { "src", a_source }, { "step", fp.step },
            { "stride", { { "skill", stride.sneakSkill }, { "gait", GaitName(stride.gait) },
                            { "boots", stride.heavyBoots }, { "body", stride.heavyBody },
                            { "over", stride.overEncumbered }, { "lightFoot", stride.lightFoot },
                            { "race", RaceName(stride.race) }, { "beast", stride.beastForm } } } });

        SKSE::log::trace("Trail: #{} via {} at ({:.0f}, {:.0f}, {:.0f}) space {:08X} material {:08X} step {:.2f}{}",
            fp.seq, a_source, pos.x, pos.y, pos.z, space,
            static_cast<std::uint32_t>(fp.material), fp.step, (fp.flags & kBlood) ? " blood" : "");
    }

    void Init() {
        auto* data = RE::TESDataHandler::GetSingleton();
        if (!data) {
            return;
        }
        g_lightFoot   = data->LookupForm<RE::BGSPerk>(kLightFootPerk, "Skyrim.esm");
        g_werewolf    = data->LookupForm<RE::TESRace>(kWerewolfRace, "Skyrim.esm");
        g_vampireLord = data->LookupForm<RE::TESRace>(kVampireLordRace, "Dawnguard.esm");
        const auto& cfg = Config::Get().step;
        g_raceKinds.clear();
        AddRaces(data, cfg.khajiit_races, RaceKind::kKhajiit, "khajiit");
        AddRaces(data, cfg.argonian_races, RaceKind::kArgonian, "argonian");
        AddRaces(data, cfg.bosmer_races, RaceKind::kBosmer, "bosmer");
        SKSE::log::info("Trail: Light Foot {}, werewolf {}, vampire lord {}, {} races with a step of their own",
            g_lightFoot ? "found" : "missing", g_werewolf ? "found" : "missing",
            g_vampireLord ? "found" : "missing", g_raceKinds.size());
    }

    void MarkPlayerHit() {
        g_bloodSteps = Config::Get().trail.blood_steps;
    }

    float Visibility(const Footprint& a_fp, float a_nowHours, bool a_badWeather) {
        return Visibility(a_fp, a_nowHours, a_badWeather, Config::Get().trail);
    }

    void Prune(float a_nowHours, float a_minVisibility) {
        Prune(g_trail, a_nowHours, a_minVisibility, Config::Get().trail);
    }

    const Footprint* FindBySeq(std::uint32_t a_seq) {
        return FindBySeq(g_trail, a_seq);
    }

    const std::deque<Footprint>& Footprints() {
        return g_trail;
    }

    void Clear() {
        g_trail.clear();
        g_bloodSteps = 0;
    }

    void Restore(const Footprint& a_fp) {
        auto fp = a_fp;
        fp.seq  = g_nextSeq++;
        g_trail.push_back(fp);
    }

    void Inject(const RE::NiPoint3& a_pos, RE::FormID a_space, RE::MATERIAL_ID a_material, std::string_view a_source) {
        const auto* calendar = RE::Calendar::GetSingleton();
        if (!calendar || a_space == 0) {
            return;
        }
        Footprint fp;
        fp.pos       = a_pos;
        fp.space     = a_space;
        fp.gameHours = calendar->GetHoursPassed();
        fp.material  = a_material;
        fp.seq       = g_nextSeq++;
        g_trail.push_back(fp);
        const auto& cfg = Config::Get().trail;
        if (g_trail.size() > static_cast<std::size_t>(cfg.max_footprints)) {
            Thin(g_trail, cfg);
        }
        Telemetry::Write({ { "type", "footprint" }, { "seq", fp.seq }, { "pos", Telemetry::Vec(a_pos) },
            { "space", Telemetry::Hex(a_space) }, { "mat", Telemetry::Hex(static_cast<std::uint32_t>(a_material)) },
            { "base", Visibility(fp, fp.gameHours, false) }, { "blood", false }, { "src", a_source } });
    }

    std::string DescribeStep(RE::PlayerCharacter* a_player) {
        if (!a_player) {
            return {};
        }
        const auto  material = SurfaceUnder(a_player);
        const auto* cell     = a_player->GetParentCell();
        const auto  s        = ReadStride(a_player, material, cell && cell->IsInteriorCell());
        auto text = std::format("{} x{:.2f}  skill {:.0f} {}", MaterialName(material), StepMult(s, Config::Get().step),
            s.sneakSkill, GaitName(s.gait));
        for (const auto& [on, name] : { std::pair{ s.heavyBoots, "boots" }, { s.heavyBody, "body" },
                 { s.overEncumbered, "over" }, { s.lightFoot, "lightFoot" }, { s.beastForm, "beast" } }) {
            if (on) {
                text += ' ';
                text += name;
            }
        }
        if (s.race != RaceKind::kOther) {
            text += ' ';
            text += RaceName(s.race);
        }
        if (!OnGround(a_player)) {
            text += "  AIR";
        } else if (g_vampireLord && a_player->GetRace() == g_vampireLord && Levitating(a_player)) {
            text += "  LEVITATING";
        }
        return text;
    }

    std::string_view MaterialName(RE::MATERIAL_ID a_material) {
        using M = RE::MATERIAL_ID;
        switch (a_material) {
        case M::kNone:               return "none";
        case M::kStone:              return "stone";
        case M::kStoneBroken:        return "stoneBroken";
        case M::kStoneHeavy:         return "stoneHeavy";
        case M::kStoneStairs:        return "stoneStairs";
        case M::kStoneStairsBroken:  return "stoneStairsBroken";
        case M::kStoneAsStairs:      return "stoneAsStairs";
        case M::kSnow:               return "snow";
        case M::kSnowStairs:         return "snowStairs";
        case M::kIce:                return "ice";
        case M::kMud:                return "mud";
        case M::kDirt:               return "dirt";
        case M::kGrass:              return "grass";
        case M::kSand:               return "sand";
        case M::kAsh:                return "ash";
        case M::kGravel:             return "gravel";
        case M::kWood:               return "wood";
        case M::kWoodStairs:         return "woodStairs";
        case M::kWoodAsStairs:       return "woodAsStairs";
        case M::kWoodHeavy:          return "woodHeavy";
        case M::kCarpet:             return "carpet";
        case M::kWater:              return "water";
        case M::kWaterPuddle:        return "puddle";
        case M::kMetalSolid:         return "metal";
        case M::kGlass:              return "glass";
        default:                     return "other";
        }
    }

    bool FootstepEventsActive() {
        return Clock::now() - g_lastFootstepEvent < 1s;
    }

    void NoteFootstepEvent() {
        g_lastFootstepEvent = Clock::now();
    }
}
