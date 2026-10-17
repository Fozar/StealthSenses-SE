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

// 0.11: sense profiles — how well an NPC reads the player's trail. The pure part: parsing the
// profile files and merging the rules that match an NPC. Profiles.cpp matches rules against
// actors; the unit tests (tests/) call this directly.
namespace StealthSenses::Profiles {
    // 1 = a human as before 0.11
    struct Profile {
        float       tracking = 1.0f;  // reads footprints: visibility and notice radius times this; 0 = never
        float       night    = 1.0f;  // share of the darkness penalty it takes; 0 = sees in the dark
        float       blood    = 1.0f;  // bloody footprints: visibility and notice radius times this on top
        std::string name     = "default";  // names of the rules that made it, "+"-joined

        bool operator==(const Profile&) const = default;
    };

    inline constexpr float kMaxTracking = 3.0f;
    inline constexpr float kMaxBlood    = 5.0f;

    // What a rule matches on, least specific first: a later layer overrides an earlier one
    enum class Layer : std::uint8_t { kKeyword, kRace, kClass, kFaction, kNpc };

    // The fields one rule sets; the rest come from the layers below. "tracking"/"blood" set a
    // value, "tracking_mult"/"blood_mult" multiply what the layers below made (a faction of hunters
    // on top of a race: a Bosmer hunter keeps the Bosmer bonus).
    struct Patch {
        std::optional<float> tracking;
        std::optional<float> night;
        std::optional<float> blood;
        std::optional<float> trackingMult;
        std::optional<float> bloodMult;

        void ApplyTo(Profile& a_profile) const;
    };

    struct Rule {
        Layer       layer = Layer::kKeyword;
        std::string form;  // "Plugin.esm|FormID", resolved by Profiles.cpp
        std::string name;
        Patch       patch;
    };

    struct RuleSet {
        Patch             base;  // "default": what an NPC no rule matches gets
        std::vector<Rule> rules;  // in file order
    };

    // Adds one profiles file to a_set: its "default" fields over the ones before, its rules after
    // them. A rule without exactly one match key or with no name is skipped with a warning; values
    // are clamped. Throws nlohmann::json::exception on malformed JSON.
    void Parse(std::string_view a_text, RuleSet& a_set, std::vector<std::string>& a_warnings);

    // The profile from the base and the rules that match an NPC, by layer and then in file order
    Profile Resolve(const Patch& a_base, std::span<const Rule* const> a_matched);

    // A footprint's visibility as this NPC reads it: the trail's own visibility (Trail::Visibility)
    // times tracking, times blood on a bloody one, less the darkness it does not see through.
    // a_darkness 0..1 (TrailLogic Darkness), a_nightPenalty: visibility lost in full darkness.
    float Perceived(float a_visibility, bool a_blood, float a_darkness, float a_nightPenalty, const Profile& a_profile);

    // How far it notices a footprint: the base radius times tracking (and blood), at most kMaxRadiusMult
    inline constexpr float kMaxRadiusMult = 3.0f;
    float NoticeRadius(float a_base, bool a_blood, const Profile& a_profile);
}
