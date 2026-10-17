// SPDX-License-Identifier: GPL-3.0-or-later

#include "Config.h"
#include "ProfileLogic.h"
#include "TrailLogic.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace StealthSenses;
using Profiles::Layer;

namespace {
    Profiles::RuleSet ParseOne(std::string_view a_text, std::vector<std::string>* a_warnings = nullptr) {
        Profiles::RuleSet        set;
        std::vector<std::string> warnings;
        Profiles::Parse(a_text, set, warnings);
        if (a_warnings) {
            *a_warnings = std::move(warnings);
        }
        return set;
    }

    const Profiles::Rule* Named(const Profiles::RuleSet& a_set, std::string_view a_name) {
        for (const auto& rule : a_set.rules) {
            if (rule.name == a_name) {
                return &rule;
            }
        }
        return nullptr;
    }
}

TEST_CASE("profiles: rules are read with their layer and form") {
    const auto set = ParseOne(R"({
        "default": { "tracking": 0 },
        "rules": [
            { "keyword": "Skyrim.esm|013794", "name": "person", "tracking": 1 },
            { "faction": "Skyrim.esm|04E509", "name": "hunter", "tracking_mult": 1.3, "why": "ignored" }
        ]
    })");
    CHECK(set.base.tracking == 0.0f);
    REQUIRE(set.rules.size() == 2);
    CHECK(set.rules[0].layer == Layer::kKeyword);
    CHECK(set.rules[0].form == "Skyrim.esm|013794");
    CHECK(set.rules[1].layer == Layer::kFaction);
    CHECK(set.rules[1].patch.trackingMult == 1.3f);
    CHECK_FALSE(set.rules[1].patch.tracking);
}

TEST_CASE("profiles: broken rules are skipped with a warning, values clamped") {
    std::vector<std::string> warnings;
    const auto               set = ParseOne(R"({
        "rules": [
            { "race": "Skyrim.esm|013749" },
            { "race": "Skyrim.esm|013749", "faction": "Skyrim.esm|04E509", "name": "two keys" },
            { "name": "no key", "tracking": 2 },
            { "race": 42, "name": "not a string" },
            "not an object",
            { "race": "Skyrim.esm|013749", "name": "loud", "tracking": 99, "night": -1, "blood": "lots" }
        ]
    })", &warnings);
    REQUIRE(set.rules.size() == 1);
    const auto& loud = set.rules[0];
    CHECK(loud.patch.tracking == Profiles::kMaxTracking);
    CHECK(loud.patch.night == 0.0f);
    CHECK_FALSE(loud.patch.blood);
    CHECK(warnings.size() == 6);
}

TEST_CASE("profiles: layers apply keyword < race < class < faction < npc, then file order") {
    const auto set = ParseOne(R"({
        "default": { "tracking": 0 },
        "rules": [
            { "faction": "A|1", "name": "hunter", "tracking_mult": 1.5 },
            { "race": "A|2", "name": "bosmer", "tracking": 1.4, "night": 0.5 },
            { "keyword": "A|3", "name": "person", "tracking": 1 },
            { "keyword": "A|4", "name": "vampire", "night": 0, "blood": 2 }
        ]
    })");
    const Profiles::Rule* all[] = { Named(set, "hunter"), Named(set, "bosmer"), Named(set, "person"), Named(set, "vampire") };
    const auto            p     = Profiles::Resolve(set.base, all);
    // The faction multiplies what the race set (the keyword's 1 replaced by the race's 1.4)
    CHECK(p.tracking == doctest::Approx(1.4f * 1.5f));
    // The race's night 0.5 comes after the vampire keyword's 0
    CHECK(p.night == 0.5f);
    CHECK(p.blood == 2.0f);
    CHECK(p.name == "person+vampire+bosmer+hunter");

    // No rule: the default — tracking 0, never reads
    CHECK(Profiles::Resolve(set.base, {}).tracking == 0.0f);
    CHECK(Profiles::Resolve(set.base, {}).name == "default");
}

TEST_CASE("profiles: later files override the default and add rules after") {
    Profiles::RuleSet        set;
    std::vector<std::string> warnings;
    Profiles::Parse(R"({ "default": { "tracking": 0, "night": 1 }, "rules": [ { "race": "A|1", "name": "a", "tracking": 1 } ] })", set, warnings);
    Profiles::Parse(R"({ "default": { "night": 0.5 }, "rules": [ { "race": "A|1", "name": "b", "tracking": 2 } ] })", set, warnings);
    CHECK(set.base.tracking == 0.0f);
    CHECK(set.base.night == 0.5f);
    const Profiles::Rule* both[] = { &set.rules[0], &set.rules[1] };
    CHECK(Profiles::Resolve(set.base, both).tracking == 2.0f);
    CHECK(warnings.empty());
}

TEST_CASE("profiles: perceived visibility — tracking, blood, darkness") {
    const Profiles::Profile human;
    CHECK(Profiles::Perceived(0.6f, false, 0.0f, 0.5f, human) == doctest::Approx(0.6f));
    // Full darkness takes night_penalty off
    CHECK(Profiles::Perceived(0.6f, false, 1.0f, 0.5f, human) == doctest::Approx(0.3f));

    Profiles::Profile cat;
    cat.tracking = 1.1f;
    cat.night    = 0.0f;
    CHECK(Profiles::Perceived(0.6f, false, 1.0f, 0.5f, cat) == doctest::Approx(0.66f));

    Profiles::Profile vampire;
    vampire.night = 0.0f;
    vampire.blood = 2.0f;
    CHECK(Profiles::Perceived(0.6f, true, 1.0f, 0.5f, vampire) == doctest::Approx(1.2f));
    CHECK(Profiles::Perceived(0.6f, false, 1.0f, 0.5f, vampire) == doctest::Approx(0.6f));

    Profiles::Profile blind;
    blind.tracking = 0.0f;
    CHECK(Profiles::Perceived(2.0f, true, 0.0f, 0.5f, blind) == 0.0f);
}

TEST_CASE("profiles: notice radius grows with tracking and blood, capped") {
    Profiles::Profile p;
    CHECK(Profiles::NoticeRadius(350.0f, false, p) == doctest::Approx(350.0f));
    p.tracking = 1.4f;
    CHECK(Profiles::NoticeRadius(350.0f, false, p) == doctest::Approx(490.0f));
    p.blood = 5.0f;
    CHECK(Profiles::NoticeRadius(350.0f, true, p) == doctest::Approx(350.0f * Profiles::kMaxRadiusMult));
}

TEST_CASE("darkness: by the hour outside, constant inside") {
    const Config::DarkSettings cfg;  // dusk 18, night 21, dawn 5, day 7
    CHECK(Trail::Darkness(12.0f, true, cfg) == 0.0f);
    CHECK(Trail::Darkness(0.0f, true, cfg) == 1.0f);
    CHECK(Trail::Darkness(23.0f, true, cfg) == 1.0f);
    CHECK(Trail::Darkness(19.5f, true, cfg) == doctest::Approx(0.5f));
    CHECK(Trail::Darkness(6.0f, true, cfg) == doctest::Approx(0.5f));
    CHECK(Trail::Darkness(7.0f, true, cfg) == 0.0f);
    CHECK(Trail::Darkness(18.0f, true, cfg) == 0.0f);
    CHECK(Trail::Darkness(12.0f, false, cfg) == doctest::Approx(cfg.interior));
    CHECK(Trail::Darkness(0.0f, false, cfg) == doctest::Approx(cfg.interior));

    Config::DarkSettings off;
    off.enabled = false;
    CHECK(Trail::Darkness(0.0f, true, off) == 0.0f);
}

TEST_CASE("profiles: the shipped StealthSensesProfiles.json reads without warnings") {
    std::ifstream file(STEALTHSENSES_SOURCE_DIR "/StealthSensesProfiles.json");
    REQUIRE(file);
    const std::string        text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    Profiles::RuleSet        set;
    std::vector<std::string> warnings;
    Profiles::Parse(text, set, warnings);
    for (const auto& w : warnings) {
        FAIL_CHECK(w);
    }
    // Every form reference is well-formed, and the default keeps creatures off the trail
    for (const auto& rule : set.rules) {
        CHECK_MESSAGE(Config::ParseFormRef(rule.form).has_value(), rule.name);
    }
    CHECK(set.base.tracking == 0.0f);

    // A plain person reads as before 0.11
    const Profiles::Rule* person[] = { Named(set, "person") };
    REQUIRE(person[0]);
    CHECK(Profiles::Resolve(set.base, person).tracking == 1.0f);
    CHECK(Profiles::Resolve(set.base, person).night == 1.0f);
    CHECK(Profiles::Resolve(set.base, person).blood == 1.0f);
}
