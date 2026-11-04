// SPDX-License-Identifier: GPL-3.0-or-later
#include "ScentLogic.h"
#include "ProfileLogic.h"
#include <doctest/doctest.h>
using namespace StealthSenses;

TEST_CASE("weather affects perception, not body and blood sources") {
    const Config::ScentSettings cfg;
    CHECK(Scent::WeatherFactor(false, true, true, cfg) == 1);
    CHECK(Scent::WeatherFactor(true, false, false, cfg) == 1);
    CHECK(Scent::WeatherFactor(true, true, false, cfg) == doctest::Approx(0.8));
    CHECK(Scent::WeatherFactor(true, false, true, cfg) == doctest::Approx(0.85));
    CHECK(Scent::WeatherFactor(true, true, true, cfg) == doctest::Approx(0.8));
    Trail::Footprint fp{}; fp.scentSegment = 1; fp.scentBody = 0.5f; fp.flags = Trail::kBlood;
    const auto source = Scent::SampleAt(fp, 0, cfg);
    Profiles::Profile profile; profile.smell = 1;
    CHECK(Scent::Perceived(source, 0, true, profile, cfg, 0.8f) == doctest::Approx(source.peak * 0.8f));
    CHECK(Scent::SampleAt(fp, 0, cfg).peak == source.peak);
    CHECK(fp.scentBody == 0.5f);
}

TEST_CASE("scent weather config defaults types and bounds") {
    const auto defaults = Config::Parse("{}").scent;
    CHECK(defaults.rain_mult == doctest::Approx(0.8));
    CHECK(defaults.snow_mult == doctest::Approx(0.85));
    const auto bad = Config::Parse(R"({"scent":{"rain_mult":true,"snow_mult":"bad"}})").scent;
    CHECK(bad.rain_mult == defaults.rain_mult);
    CHECK(bad.snow_mult == defaults.snow_mult);
    const auto bounded = Config::Parse(R"({"scent":{"rain_mult":-1,"snow_mult":2}})").scent;
    CHECK(bounded.rain_mult == 0);
    CHECK(bounded.snow_mult == 1);
}

TEST_CASE("body scent is a clothing and sprint snapshot; blood remains independent") {
    Config::ScentSettings cfg;
    CHECK(Scent::BodyFactor(false, false, false, false, false, cfg) == doctest::Approx(1));
    CHECK(Scent::BodyFactor(true, true, true, true, false, cfg) == doctest::Approx(0.7));
    CHECK(Scent::BodyFactor(false, false, false, false, true, cfg) == doctest::Approx(1.5));
    Trail::Footprint fp;
    fp.scentSegment = 1;
    fp.scentBody = Scent::BodyFactor(true, true, true, true, true, cfg);
    CHECK(fp.scentBody == doctest::Approx(1.05));
    fp.flags = Trail::kBlood;
    CHECK(Scent::Strength(fp, 0, cfg) == doctest::Approx(2.05));
    cfg.body_torso_reduction = 0;
    CHECK(Scent::Strength(fp, 0, cfg) == doctest::Approx(2.05));
    cfg.base_strength = 2;
    CHECK(Scent::Strength(fp, 0, cfg) == doctest::Approx(3.1));
}

TEST_CASE("scent reader: a blocked nearest source does not hide a clear candidate within the ray budget") {
    Trail::Footprint blocked, clear;
    blocked.seq = 1; clear.seq = 2;
    const Trail::Footprint* sources[]{ &blocked, &clear };
    auto ray = [](const Trail::Footprint& fp) { return fp.seq == 2; };
    CHECK(Scent::FirstReachable(sources, ray, 6) == &clear);
    CHECK(Scent::FirstReachable(sources, ray, 1) == nullptr);
    CHECK(Scent::FirstReachable(sources, ray, 0) == nullptr);
    CHECK(Scent::FirstReachable({}, ray, 6) == nullptr);
}

TEST_CASE("scent reader: sensitivity changes range without reviving expired sources") {
    Config::ScentSettings cfg;
    Profiles::Profile p;
    p.smell = 2;
    CHECK(Scent::Perceived({1, 450}, 450, false, p, cfg) == doctest::Approx(0.5));
    CHECK(Scent::Perceived({0.05f, 450}, 0, false, p, cfg) < cfg.min_strength);
    p.smell = 0;
    CHECK(Scent::Perceived({2, 450}, 0, true, p, cfg) == 0);
    p.smell = 0.08f;
    CHECK(Scent::Perceived({1, 450}, 40, false, p, cfg) == 0);
    p.smell = 1.6f;
    p.smellBlood = 4;
    CHECK(Scent::Perceived({2, 450}, 900, true, p, cfg) == doctest::Approx(1));
    CHECK(Scent::Perceived({1, 450}, 900, false, p, cfg) == 0);
    p.tracking = 0; p.night = 1; p.blood = 0;
    CHECK(Scent::Perceived({2, 450}, 900, true, p, cfg) == doctest::Approx(1));
    cfg.enabled = false;
    CHECK(Scent::Perceived({2, 450}, 0, true, p, cfg) == 0);
}

TEST_CASE("scent reader: continuation never crosses water or legacy segments") {
    Trail::Footprint fp;
    fp.seq = 12; fp.scentSegment = 3;
    CHECK(Scent::Ahead(fp, 11, 3));
    CHECK_FALSE(Scent::Ahead(fp, 12, 3));
    CHECK_FALSE(Scent::Ahead(fp, 11, 2));
    CHECK(Scent::Ahead(fp, 0, 0)); // first pickup chooses its segment
    fp.scentSegment = 0;
    CHECK_FALSE(Scent::Ahead(fp, 0, 0));
}

namespace {
    Trail::Footprint Source() {
        Trail::Footprint fp;
        fp.space = 0x3C;
        fp.gameHours = 10.0f;
        fp.scentSegment = 1;
        return fp;
    }
}

TEST_CASE("scent: decay depends on age and blood, not material or stride") {
    Config::ScentSettings cfg;
    cfg.spread_max_mult = 1; // isolate aging from dilution
    auto fp = Source();
    CHECK(Scent::Strength(fp, 10.0f, cfg) == doctest::Approx(1.0f));
    CHECK(Scent::Strength(fp, 10.5f, cfg) == doctest::Approx(0.5f));
    CHECK(Scent::Strength(fp, 11.0f, cfg) == doctest::Approx(0.25f));
    CHECK(Scent::Strength(fp, 9.0f, cfg) == doctest::Approx(1.0f));
    fp.material = RE::MATERIAL_ID::kStone;
    fp.step = 0.25f;
    CHECK(Scent::Strength(fp, 10.0f, cfg) == doctest::Approx(1.0f));
    fp.flags |= Trail::kBlood;
    CHECK(Scent::Strength(fp, 10.5f, cfg) == doctest::Approx(1.0f));
}

TEST_CASE("scent: default dispersion dilutes the peak as well as aging it") {
    const auto fp = Source();
    const Config::ScentSettings cfg;
    CHECK(Scent::Strength(fp, 10.1f, cfg) == doctest::Approx(0.649f).epsilon(0.001));
    CHECK(Scent::Strength(fp, 10.5f, cfg) == doctest::Approx(0.32086f).epsilon(0.001));
    CHECK(Scent::Strength(fp, 11.5f, cfg) < cfg.min_strength);
}

TEST_CASE("scent: spread is bounded, peak and integrated amount fall, and perception uses the grown sample") {
    const auto fp = Source();
    const Config::ScentSettings cfg;
    const auto fresh = Scent::SampleAt(fp, 10, cfg);
    const auto expanded = Scent::SampleAt(fp, 10.1f, cfg);
    CHECK(fresh.radius == 450);
    CHECK(expanded.radius == doctest::Approx(521.11356f).epsilon(0.0001));
    CHECK(Scent::Radius(expanded, cfg) == doctest::Approx(440.8f).epsilon(0.001));
    CHECK(Scent::AtDistance(fresh, 430, cfg) < cfg.min_strength);
    CHECK(Scent::AtDistance(expanded, 430, cfg) > cfg.min_strength);
    Profiles::Profile wolf; wolf.smell = 1.6f;
    CHECK(Scent::Perceived(expanded, 688, false, wolf, cfg) == doctest::Approx(Scent::AtDistance(expanded, 430, cfg)));
    float prevPeak = fresh.peak, prevRadius = fresh.radius;
    for (float age : {0.1f, 0.5f, 1.0f, 1.5f, 100.0f}) {
        const auto s = Scent::SampleAt(fp, 10 + age, cfg);
        CHECK(s.radius >= prevRadius);
        CHECK(s.radius <= 562.5f);
        CHECK(s.peak <= prevPeak);
        prevPeak = s.peak; prevRadius = s.radius;
    }
    const auto half = Scent::SampleAt(fp, 10.5f, cfg);
    CHECK(half.peak * half.radius * half.radius / (450 * 450) == doctest::Approx(0.5));
    CHECK(Scent::Radius(Scent::SampleAt(fp, 11.5f, cfg), cfg) == 0);
    CHECK(Scent::SampleAt(fp, 9, cfg).radius == 450);
    CHECK(Scent::SampleAt(fp, 9, cfg).peak == 1);
    auto blood = fp; blood.flags |= Trail::kBlood;
    const auto bleeding = Scent::SampleAt(blood, 10.5f, cfg);
    CHECK(bleeding.peak == doctest::Approx(0.64173f).epsilon(0.001));
    CHECK(bleeding.radius == half.radius);
}

TEST_CASE("scent: invalid spread parameters cannot generate concentration or a radius") {
    const auto fp = Source();
    Config::ScentSettings cfg;
    for (float bad : {0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        cfg.spread_hours = bad;
        CHECK(Scent::SampleAt(fp, 10.1f, cfg).peak == 0);
        CHECK(Scent::SampleAt(fp, 10.1f, cfg).radius == 0);
    }
    cfg = {};
    cfg.spread_max_mult = 0.5f;
    CHECK(Scent::Strength(fp, 10.1f, cfg) == 0);
    cfg.spread_max_mult = 1;
    CHECK(Scent::SampleAt(fp, 10.5f, cfg).peak == doctest::Approx(0.5));
    CHECK(Scent::SampleAt(fp, 10.5f, cfg).radius == 450);
}

TEST_CASE("scent: the potential radius shrinks with age and has a linear radial falloff") {
    const Config::ScentSettings cfg;
    CHECK(Scent::AtDistance({1.0f, 450}, 225.0f, cfg) == doctest::Approx(0.5f));
    CHECK(Scent::AtDistance({1.0f, 450}, 450.0f, cfg) == 0.0f);
    CHECK(Scent::AtDistance({1.0f, 450}, 600.0f, cfg) == 0.0f);
    CHECK(Scent::Radius({1.0f, 450}, cfg) == doctest::Approx(405.0f));
    CHECK(Scent::Radius({0.25f, 450}, cfg) == doctest::Approx(270.0f));
    CHECK(Scent::Radius({0.1f, 450}, cfg) == 0.0f);
    CHECK(Scent::Radius({0.05f, 450}, cfg) == 0.0f);
}

TEST_CASE("scent: legacy, disabled and invalid sources have no concentration") {
    auto fp = Source();
    Config::ScentSettings cfg;
    cfg.enabled = false;
    CHECK(Scent::Strength(fp, 10, cfg) == 0.0f);
    cfg.enabled = true;
    fp.scentSegment = 0;
    CHECK(Scent::Strength(fp, 10, cfg) == 0.0f);
    fp = Source();
    fp.gameHours = std::numeric_limits<float>::quiet_NaN();
    CHECK(Scent::Strength(fp, 10, cfg) == 0.0f);
    CHECK(Scent::AtDistance({1, 450}, std::numeric_limits<float>::infinity(), cfg) == 0.0f);
    CHECK(Scent::Radius({std::numeric_limits<float>::quiet_NaN(), 450}, cfg) == 0.0f);
}

TEST_CASE("scent: only the same nonzero segment in the same space connects") {
    auto a = Source(), b = Source();
    CHECK(Scent::Connected(a, b));
    b.scentSegment = 2;
    CHECK_FALSE(Scent::Connected(a, b));
    b = Source(); b.space = 0x123;
    CHECK_FALSE(Scent::Connected(a, b));
    a.scentSegment = b.scentSegment = 0;
    CHECK_FALSE(Scent::Connected(a, b));
}

TEST_CASE("scent: a contact break persists until the next point, even at the same position") {
    Scent::Chain chain;
    const auto first = chain.Stamp(0x3C);
    CHECK(first != 0);
    CHECK(chain.Stamp(0x3C) == first);
    chain.Break(); // water observed between recorded steps, before any spacing check
    chain.Break(); // repeated observations do not consume identifiers
    const auto next = chain.Stamp(0x3C);
    CHECK(next == first + 1);
    CHECK(chain.Stamp(0x123) != next);
    CHECK(chain.Stamp(0) == 0);
}

TEST_CASE("scent: restored runs keep boundaries and cannot join new live steps") {
    Scent::Chain chain;
    const auto a = chain.Restore(std::numeric_limits<std::uint32_t>::max(), 0x3C);
    CHECK(a == 1); // saved IDs are remapped, not used to advance a counter
    CHECK(chain.Restore(std::numeric_limits<std::uint32_t>::max(), 0x3C) == a);
    const auto b = chain.Restore(12, 0x3C);
    CHECK(b != a);
    CHECK(chain.Restore(0, 0x3C) == 0); // legacy visual-only point
    CHECK(chain.Restore(12, 0x3C) != b);
    chain.Break(); // OnGameLoaded
    CHECK(chain.Stamp(0x3C) > b);
}
