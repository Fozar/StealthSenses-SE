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

#include "Profiles.h"

#include "Config.h"

#include <nlohmann/json.hpp>

namespace StealthSenses::Profiles {
    namespace {
        struct Resolved {
            const Rule*   rule = nullptr;
            RE::TESForm*  form = nullptr;
        };

        RuleSet               g_set;
        std::vector<Resolved> g_rules;
        float                 g_maxBoost = 1.0f;

        bool Matches(RE::Actor* a_actor, const Resolved& a_r) {
            switch (a_r.rule->layer) {
            case Layer::kKeyword:
                // Actor::HasKeyword is the engine's helper: race keywords count (ActorTypeNPC sits
                // on the race and filtered humanoids through 0.10)
                return a_actor->HasKeyword(a_r.form->As<RE::BGSKeyword>());
            case Layer::kRace:
                return a_actor->GetRace() == a_r.form;
            case Layer::kClass: {
                const auto* base = a_actor->GetActorBase();
                return base && base->npcClass == a_r.form;
            }
            case Layer::kFaction:
                return a_actor->IsInFaction(a_r.form->As<RE::TESFaction>());
            case Layer::kNpc:
                return a_actor->GetActorBase() == a_r.form;
            }
            return false;
        }

        bool RightType(Layer a_layer, const RE::TESForm* a_form) {
            switch (a_layer) {
            case Layer::kKeyword: return a_form->Is(RE::FormType::Keyword);
            case Layer::kRace:    return a_form->Is(RE::FormType::Race);
            case Layer::kClass:   return a_form->Is(RE::FormType::Class);
            case Layer::kFaction: return a_form->Is(RE::FormType::Faction);
            case Layer::kNpc:     return a_form->Is(RE::FormType::NPC);
            }
            return false;
        }

        void ReadFile(const std::filesystem::path& a_path) {
            std::ifstream file(a_path);
            if (!file) {
                SKSE::log::warn("Profiles: cannot open {}", a_path.string());
                return;
            }
            const std::string        text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
            std::vector<std::string> warnings;
            const auto               before = g_set.rules.size();
            try {
                Parse(text, g_set, warnings);
            } catch (const nlohmann::json::exception& e) {
                SKSE::log::error("Profiles: {} is not valid JSON, skipped: {}", a_path.filename().string(), e.what());
                return;
            }
            for (const auto& w : warnings) {
                SKSE::log::warn("Profiles: {}: {}", a_path.filename().string(), w);
            }
            SKSE::log::info("Profiles: {} — {} rules", a_path.filename().string(), g_set.rules.size() - before);
        }
    }

    void Load() {
        g_set   = {};
        g_rules.clear();

        const std::filesystem::path dir = "Data/SKSE/Plugins";
        const auto                  main = dir / "StealthSensesProfiles.json";
        if (std::filesystem::exists(main)) {
            ReadFile(main);
        } else {
            SKSE::log::warn("Profiles: {} not found: every NPC gets the built-in default", main.string());
        }
        // Other mods' additions after ours, so they override it
        std::vector<std::filesystem::path> extra;
        std::error_code                    ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const auto name = entry.path().filename().string();
            if (entry.is_regular_file() && name.starts_with("StealthSensesProfiles_") && name.ends_with(".json")) {
                extra.push_back(entry.path());
            }
        }
        std::ranges::sort(extra);
        for (const auto& path : extra) {
            ReadFile(path);
        }

        // Rules naming a plugin that is not loaded (a DLC, a mod) are expected: skipped quietly
        auto*       data       = RE::TESDataHandler::GetSingleton();
        std::size_t unresolved = 0;
        float       maxTracking = 1.0f;
        float       maxBlood    = 1.0f;
        for (const auto& rule : g_set.rules) {
            const auto ref = Config::ParseFormRef(rule.form);
            if (!ref) {
                SKSE::log::warn("Profiles: rule '{}': '{}' is not \"Plugin.esm|FormID\"", rule.name, rule.form);
                ++unresolved;
                continue;
            }
            auto* form = data ? data->LookupForm(ref->second, ref->first) : nullptr;
            if (!form) {
                SKSE::log::debug("Profiles: rule '{}': {} not loaded", rule.name, rule.form);
                ++unresolved;
                continue;
            }
            if (!RightType(rule.layer, form)) {
                SKSE::log::warn("Profiles: rule '{}': {} is a {}, not what the rule matches on", rule.name, rule.form,
                    RE::FormTypeToString(form->GetFormType()));
                ++unresolved;
                continue;
            }
            g_rules.push_back({ &rule, form });
            maxTracking = std::max(maxTracking, rule.patch.tracking.value_or(0.0f));
            maxBlood    = std::max(maxBlood, rule.patch.blood.value_or(0.0f));
        }
        maxTracking = std::max(maxTracking, g_set.base.tracking.value_or(1.0f));
        maxBlood    = std::max(maxBlood, g_set.base.blood.value_or(1.0f));
        g_maxBoost  = maxTracking * maxBlood;
        SKSE::log::info("Profiles: {} rules in use, {} not resolved; most a profile boosts a footprint: x{:.2f}",
            g_rules.size(), unresolved, g_maxBoost);
    }

    Profile For(RE::Actor* a_actor) {
        std::vector<const Rule*> matched;
        if (a_actor) {
            for (const auto& r : g_rules) {
                if (Matches(a_actor, r)) {
                    matched.push_back(r.rule);
                }
            }
        }
        return Resolve(g_set.base, matched);
    }

    float MaxBoost() {
        return g_maxBoost;
    }
}
