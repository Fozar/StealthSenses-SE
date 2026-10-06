// SPDX-License-Identifier: GPL-3.0-or-later

#include "Config.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

using namespace StealthSenses;

TEST_CASE("config: empty object gives the defaults") {
    CHECK(Config::Parse("{}") == Config::Settings{});
}

TEST_CASE("config: the shipped StealthSensesConfig.json matches the defaults in Config.h") {
    // CLAUDE.md: defaults live in Config.h and in the JSON, kept in sync by hand. This holds them.
    std::ifstream     file(STEALTHSENSES_SOURCE_DIR "/StealthSensesConfig.json");
    REQUIRE(file);
    const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    const auto        shipped = Config::Parse(text);
    const Config::Settings defaults;
    CHECK(shipped.trail == defaults.trail);
    CHECK(shipped.tracker == defaults.tracker);
    CHECK(shipped.debug == defaults.debug);

    // ...and has every key the parser knows, so players see all of them
    const auto root = nlohmann::json::parse(text, nullptr, true, true);
    for (const char* key : { "min_spacing", "max_footprints", "thin_spacing", "halflife_hours" }) {
        CHECK_MESSAGE(root["trail"].contains(key), key);
    }
    for (const char* key : { "notice_line_of_sight", "body_found_seconds", "body_notice_radius", "body_know_radius",
             "body_max_age_hours", "give_up_seconds", "leash_distance", "tired_cooldown" }) {
        CHECK_MESSAGE(root["tracker"].contains(key), key);
    }
}

TEST_CASE("config: values are read, wrong types and unknown keys keep defaults") {
    const auto s = Config::Parse(R"({
        "trail":   { "halflife_hours": 3.5, "max_footprints": "lots", "no_such_key": 1 },
        "tracker": { "give_up_seconds": 90, "notice_line_of_sight": false },
        "debug":   { "log_level": "trace" }
    })");
    CHECK(s.trail.halflife_hours == 3.5f);
    CHECK(s.trail.max_footprints == Config::TrailSettings{}.max_footprints);
    CHECK(s.tracker.give_up_seconds == 90.0f);
    CHECK_FALSE(s.tracker.notice_line_of_sight);
    CHECK(s.debug.log_level == "trace");
}

TEST_CASE("config: comments are allowed") {
    const auto s = Config::Parse(R"({
        // players annotate their config
        "tracker": { "max_trackers": 2 /* fewer */ }
    })");
    CHECK(s.tracker.max_trackers == 2);
}

TEST_CASE("config: values that would break the loops are clamped") {
    const auto s = Config::Parse(R"({
        "trail":   { "min_spacing": 1, "max_footprints": 3, "poll_interval_ms": 1, "halflife_hours": 0, "thin_spacing": 2 },
        "tracker": { "interval_ms": 10, "max_trackers": 0 }
    })");
    CHECK(s.trail.min_spacing == 8.0f);
    CHECK(s.trail.max_footprints == 16);
    CHECK(s.trail.poll_interval_ms == 50);
    CHECK(s.trail.halflife_hours > 0.0f);
    CHECK(s.trail.thin_spacing >= s.trail.min_spacing);
    CHECK(s.tracker.interval_ms >= s.trail.poll_interval_ms);
    CHECK(s.tracker.max_trackers == 1);
}

TEST_CASE("config: malformed JSON throws (Load keeps the defaults then)") {
    CHECK_THROWS_AS(Config::Parse("{ \"trail\": "), nlohmann::json::exception);
}
