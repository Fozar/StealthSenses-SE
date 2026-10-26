// SPDX-License-Identifier: GPL-3.0-or-later

#include "Config.h"
#include "ProfileLogic.h"
#include "ScentLogic.h"
#include "TrailLogic.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace StealthSenses;
using Profiles::Layer;

TEST_CASE("profiles: scent fields merge independently, clamp and retain explicit false") {
    Profiles::RuleSet set;
    std::vector<std::string> warnings;
    Profiles::Parse(R"({"default":{"smell":0.1,"smell_hunt":true},"rules":[
      {"keyword":"A|1","name":"vampire","smell":1.6,"smell_blood":4},
      {"faction":"A|2","name":"passive","smell_mult":0.5,"smell_hunt":false}]})", set, warnings);
    const Profiles::Rule* rules[]{ &set.rules[0], &set.rules[1] };
    auto p = Profiles::Resolve(set.base, rules);
    CHECK(p.smell == doctest::Approx(0.8));
    CHECK(p.smellBlood == 4);
    CHECK_FALSE(p.smellHunt);
    CHECK(p.tracking == 1);
    Profiles::Parse(R"({"default":{"smell":99,"smell_blood":-1,"smell_hunt":"yes"}})", set, warnings);
    p = Profiles::Resolve(set.base, {});
    CHECK(p.smell == Profiles::kMaxSmell);
    CHECK(p.smellBlood == 0);
    CHECK(p.smellHunt); // malformed override leaves previous true
    CHECK(warnings.size() == 1);
}

TEST_CASE("profiles: shipped scent rules preserve ranks and undead exceptions") {
    std::ifstream file(STEALTHSENSES_SOURCE_DIR "/StealthSensesProfiles.json");
    const std::string text{ std::istreambuf_iterator<char>(file), {} };
    Profiles::RuleSet set; std::vector<std::string> warnings;
    Profiles::Parse(text, set, warnings);
    auto resolve = [&](std::string_view race, std::initializer_list<std::string_view> keywords = {}) {
        std::vector<const Profiles::Rule*> matches;
        for (const auto& rule : set.rules) {
            if ((rule.layer == Layer::kRace && rule.form == race) ||
                (rule.layer == Layer::kKeyword && std::ranges::find(keywords, rule.form) != keywords.end()))
                matches.push_back(&rule);
        }
        return Profiles::Resolve(set.base, matches);
    };
    const auto mammoth = resolve("Skyrim.esm|0131FF");
    const auto wolf = resolve("Skyrim.esm|01320A");
    const auto cat = resolve("Skyrim.esm|013200");
    const auto giant = resolve("Skyrim.esm|0131F9");
    const auto crab = resolve("Skyrim.esm|0BA545");
    CHECK(mammoth.smell > wolf.smell);
    CHECK(wolf.smell > cat.smell);
    CHECK(cat.smell > giant.smell);
    CHECK(giant.smell > crab.smell);
    CHECK(crab.smell > 0);
    CHECK(wolf.smellHunt);
    CHECK_FALSE(mammoth.smellHunt);
    const auto human = resolve("Skyrim.esm|013746", {"Skyrim.esm|013794"});
    const auto argonian = resolve("Skyrim.esm|013740", {"Skyrim.esm|013794"});
    const auto khajiit = resolve("Skyrim.esm|013745", {"Skyrim.esm|013794"});
    CHECK(khajiit.smell > argonian.smell);
    CHECK(argonian.smell > human.smell);
    CHECK(human.smell > 0);
    const auto vampire = resolve("Skyrim.esm|088845", {"Skyrim.esm|013794", "Skyrim.esm|013796", "Skyrim.esm|0A82BB"});
    CHECK(vampire.smell >= wolf.smell);
    CHECK(vampire.smellBlood > 1);
    CHECK(resolve("Dawnguard.esm|00C5F0", {"Skyrim.esm|013796"}).smell == mammoth.smell);
    for (auto race : {"Dawnguard.esm|00A2C6", "Dawnguard.esm|019D86", "Dawnguard.esm|010D00"}) {
        const auto gargoyle = resolve(race, {"Skyrim.esm|013794", "Skyrim.esm|013796", "Skyrim.esm|0A82BB"});
        CHECK_MESSAGE(gargoyle.smell == 0, race);
        CHECK_FALSE(gargoyle.smellHunt);
        CHECK(Scent::Perceived({2, 450}, 0, true, gargoyle, Config::ScentSettings{}) == 0);
    }
    for (auto race : {"Skyrim.esm|0131EF", "Skyrim.esm|0131F0", "Dragonborn.esm|035538", "Dragonborn.esm|01DCB9", "Skyrim.esm|1052A3"})
        CHECK_MESSAGE(resolve(race, {"Skyrim.esm|013794", "Skyrim.esm|013796"}).smell == 0, race);
    CHECK(Profiles::Resolve(set.base, {}).smell == 0); // no blanket Animal sniffers
}

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
