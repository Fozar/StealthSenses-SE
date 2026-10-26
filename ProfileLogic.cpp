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

#include "ProfileLogic.h"

#include <nlohmann/json.hpp>

namespace StealthSenses::Profiles {
    namespace {
        constexpr std::pair<const char*, Layer> kLayers[] = {
            { "keyword", Layer::kKeyword },
            { "race", Layer::kRace },
            { "class", Layer::kClass },
            { "faction", Layer::kFaction },
            { "npc", Layer::kNpc },
        };

        // Missing or not a number: unset. Clamped to what the trackers can take.
        std::optional<float> ReadValue(const nlohmann::json& a_obj, const char* a_key, float a_max,
            const std::string& a_where, std::vector<std::string>& a_warnings) {
            const auto it = a_obj.find(a_key);
            if (it == a_obj.end()) {
                return std::nullopt;
            }
            if (!it->is_number()) {
                a_warnings.push_back(std::format("{}: '{}' is not a number, ignored", a_where, a_key));
                return std::nullopt;
            }
            const float value = it->get<float>();
            if (!std::isfinite(value)) {
                a_warnings.push_back(std::format("{}: '{}' is not finite, ignored", a_where, a_key));
                return std::nullopt;
            }
            return std::clamp(value, 0.0f, a_max);
        }

        Patch ReadPatch(const nlohmann::json& a_obj, const std::string& a_where, std::vector<std::string>& a_warnings) {
            Patch patch;
            patch.tracking = ReadValue(a_obj, "tracking", kMaxTracking, a_where, a_warnings);
            patch.night    = ReadValue(a_obj, "night", 1.0f, a_where, a_warnings);
            patch.blood    = ReadValue(a_obj, "blood", kMaxBlood, a_where, a_warnings);
            patch.trackingMult = ReadValue(a_obj, "tracking_mult", kMaxTracking, a_where, a_warnings);
            patch.bloodMult    = ReadValue(a_obj, "blood_mult", kMaxBlood, a_where, a_warnings);
            patch.smell        = ReadValue(a_obj, "smell", kMaxSmell, a_where, a_warnings);
            patch.smellBlood   = ReadValue(a_obj, "smell_blood", kMaxSmell, a_where, a_warnings);
            patch.smellMult    = ReadValue(a_obj, "smell_mult", kMaxSmell, a_where, a_warnings);
            if (const auto it = a_obj.find("smell_hunt"); it != a_obj.end()) {
                if (it->is_boolean()) patch.smellHunt = it->get<bool>();
                else a_warnings.push_back(std::format("{}: 'smell_hunt' is not a boolean, ignored", a_where));
            }
            return patch;
        }

        void Merge(Patch& a_into, const Patch& a_from) {
            if (a_from.smell) a_into.smell = a_from.smell;
            if (a_from.smellBlood) a_into.smellBlood = a_from.smellBlood;
            if (a_from.smellMult) a_into.smellMult = a_from.smellMult;
            if (a_from.smellHunt) a_into.smellHunt = a_from.smellHunt;
            if (a_from.tracking) {
                a_into.tracking = a_from.tracking;
            }
            if (a_from.night) {
                a_into.night = a_from.night;
            }
            if (a_from.blood) {
                a_into.blood = a_from.blood;
            }
            if (a_from.trackingMult) {
                a_into.trackingMult = a_from.trackingMult;
            }
            if (a_from.bloodMult) {
                a_into.bloodMult = a_from.bloodMult;
            }
        }
    }

    void Patch::ApplyTo(Profile& a_profile) const {
        a_profile.smell = std::min(smell.value_or(a_profile.smell) * smellMult.value_or(1.0f), kMaxSmell);
        a_profile.smellBlood = smellBlood.value_or(a_profile.smellBlood);
        a_profile.smellHunt = smellHunt.value_or(a_profile.smellHunt);
        a_profile.tracking = tracking.value_or(a_profile.tracking);
        a_profile.night    = night.value_or(a_profile.night);
        a_profile.blood    = blood.value_or(a_profile.blood);
        a_profile.tracking = std::min(a_profile.tracking * trackingMult.value_or(1.0f), kMaxTracking);
        a_profile.blood    = std::min(a_profile.blood * bloodMult.value_or(1.0f), kMaxBlood);
    }

    void Parse(std::string_view a_text, RuleSet& a_set, std::vector<std::string>& a_warnings) {
        // Comments allowed, as in the config
        const auto root = nlohmann::json::parse(a_text, nullptr, true, true);
        if (!root.is_object()) {
            a_warnings.emplace_back("not a JSON object, ignored");
            return;
        }
        if (const auto it = root.find("default"); it != root.end() && it->is_object()) {
            Merge(a_set.base, ReadPatch(*it, "default", a_warnings));
        }
        const auto rules = root.find("rules");
        if (rules == root.end() || !rules->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < rules->size(); ++i) {
            const auto& obj = (*rules)[i];
            if (!obj.is_object()) {
                a_warnings.push_back(std::format("rule {}: not an object, skipped", i));
                continue;
            }
            const auto nameIt = obj.find("name");
            if (nameIt == obj.end() || !nameIt->is_string() || nameIt->get<std::string>().empty()) {
                a_warnings.push_back(std::format("rule {}: no name, skipped", i));
                continue;
            }
            Rule rule;
            rule.name = nameIt->get<std::string>();
            int keys  = 0;
            for (const auto& [key, layer] : kLayers) {
                if (const auto it = obj.find(key); it != obj.end()) {
                    ++keys;
                    rule.layer = layer;
                    rule.form  = it->is_string() ? it->get<std::string>() : std::string{};
                }
            }
            if (keys != 1 || rule.form.empty()) {
                a_warnings.push_back(std::format("rule '{}': needs exactly one of keyword/race/class/faction/npc as \"Plugin.esm|FormID\", skipped", rule.name));
                continue;
            }
            rule.patch = ReadPatch(obj, std::format("rule '{}'", rule.name), a_warnings);
            a_set.rules.push_back(std::move(rule));
        }
    }

    Profile Resolve(const Patch& a_base, std::span<const Rule* const> a_matched) {
        std::vector<const Rule*> order(a_matched.begin(), a_matched.end());
        std::ranges::stable_sort(order, {}, &Rule::layer);

        Profile profile;
        a_base.ApplyTo(profile);
        std::string name;
        for (const auto* rule : order) {
            rule->patch.ApplyTo(profile);
            name += (name.empty() ? "" : "+") + rule->name;
        }
        if (!name.empty()) {
            profile.name = std::move(name);
        }
        return profile;
    }

    float Perceived(float a_visibility, bool a_blood, float a_darkness, float a_nightPenalty, const Profile& a_profile) {
        float vis = a_visibility * a_profile.tracking;
        if (a_blood) {
            vis *= a_profile.blood;
        }
        const float dark = std::clamp(a_darkness, 0.0f, 1.0f) * std::clamp(a_nightPenalty, 0.0f, 1.0f) * a_profile.night;
        return vis * (1.0f - dark);
    }

    float NoticeRadius(float a_base, bool a_blood, const Profile& a_profile) {
        float mult = a_profile.tracking;
        if (a_blood) {
            mult *= a_profile.blood;
        }
        return a_base * std::min(mult, kMaxRadiusMult);
    }
}
