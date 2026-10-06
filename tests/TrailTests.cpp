// SPDX-License-Identifier: GPL-3.0-or-later

#include "TrailLogic.h"

#include <doctest/doctest.h>

using namespace StealthSenses;
using Trail::Footprint;

namespace {
    Footprint Make(std::uint32_t a_seq, float a_x, float a_hours = 0.0f, RE::MATERIAL_ID a_material = RE::MATERIAL_ID::kDirt,
        RE::FormID a_space = 0x3C, std::uint8_t a_flags = Trail::kNone) {
        Footprint fp;
        fp.pos       = { a_x, 0.0f, 0.0f };
        fp.space     = a_space;
        fp.gameHours = a_hours;
        fp.material  = a_material;
        fp.flags     = a_flags;
        fp.seq       = a_seq;
        return fp;
    }

    // A straight walk: one footprint every a_step units, seq from 1
    std::deque<Footprint> Walk(std::size_t a_count, float a_step) {
        std::deque<Footprint> trail;
        for (std::size_t i = 0; i < a_count; ++i) {
            trail.push_back(Make(static_cast<std::uint32_t>(i + 1), a_step * static_cast<float>(i)));
        }
        return trail;
    }
}

TEST_CASE("visibility: material base halves every halflife") {
    const Config::TrailSettings cfg;
    const auto soft = Make(1, 0, 0.0f, RE::MATERIAL_ID::kSnow);
    CHECK(Trail::Visibility(soft, 0.0f, false, cfg) == doctest::Approx(cfg.soft_visibility));
    CHECK(Trail::Visibility(soft, cfg.halflife_hours, false, cfg) == doctest::Approx(cfg.soft_visibility / 2));
    CHECK(Trail::Visibility(soft, 2 * cfg.halflife_hours, false, cfg) == doctest::Approx(cfg.soft_visibility / 4));

    CHECK(Trail::Visibility(Make(1, 0, 0, RE::MATERIAL_ID::kGravel), 0, false, cfg) == doctest::Approx(cfg.gravel_visibility));
    CHECK(Trail::Visibility(Make(1, 0, 0, RE::MATERIAL_ID::kStoneBroken), 0, false, cfg) == doctest::Approx(cfg.hard_visibility));
    CHECK(Trail::Visibility(Make(1, 0, 0, RE::MATERIAL_ID::kNone), 0, false, cfg) == doctest::Approx(cfg.unknown_visibility));
}

TEST_CASE("visibility: bad weather shortens the halflife, blood adds a bonus, no negative age") {
    const Config::TrailSettings cfg;
    const auto fp = Make(1, 0);
    CHECK(Trail::Visibility(fp, cfg.halflife_hours * cfg.weather_halflife_mult, true, cfg) == doctest::Approx(cfg.soft_visibility / 2));

    const auto bloody = Make(1, 0, 0, RE::MATERIAL_ID::kStoneBroken, 0x3C, Trail::kBlood);
    CHECK(Trail::Visibility(bloody, 0, false, cfg) == doctest::Approx(cfg.hard_visibility + cfg.blood_visibility_bonus));

    // A footprint from "the future" (game time went back on load) is fresh, not brighter
    CHECK(Trail::Visibility(Make(1, 0, 10.0f), 5.0f, false, cfg) == doctest::Approx(cfg.soft_visibility));
}

TEST_CASE("prune: drops only footprints unreadable in clear weather, anywhere in the trail") {
    const Config::TrailSettings cfg;
    std::deque<Footprint>       trail{
        Make(1, 0, 0.0f, RE::MATERIAL_ID::kStoneBroken),                // old stone: gone
        Make(2, 1, 0.0f, RE::MATERIAL_ID::kStoneBroken, 0x3C, Trail::kBlood),  // old but bloody: stays
        Make(3, 2, 0.0f, RE::MATERIAL_ID::kDirt),                       // old dirt: stays
        Make(4, 3, 9.0f, RE::MATERIAL_ID::kStoneBroken),                // fresh stone: stays
    };
    // Hour 10, halflife 2: 5 halflives = 1/32. Stone 0.15/32 = 0.005 (gone), bloody stone
    // 1.15/32 = 0.036, dirt 1/32 = 0.031, one-hour-old stone 0.106 — cut at 0.03
    Trail::Prune(trail, 10.0f, 0.03f, cfg);
    REQUIRE(trail.size() == 3);
    CHECK(trail[0].seq == 2);
    CHECK(trail[1].seq == 3);
    CHECK(trail[2].seq == 4);
}

TEST_CASE("find by seq: exact match across gaps") {
    std::deque<Footprint> trail{ Make(3, 0), Make(4, 1), Make(9, 2), Make(12, 3) };
    REQUIRE(Trail::FindBySeq(trail, 9) != nullptr);
    CHECK(Trail::FindBySeq(trail, 9)->pos.x == 2.0f);
    CHECK(Trail::FindBySeq(trail, 3) == &trail.front());
    CHECK(Trail::FindBySeq(trail, 12) == &trail.back());
    CHECK(Trail::FindBySeq(trail, 5) == nullptr);   // in a gap
    CHECK(Trail::FindBySeq(trail, 1) == nullptr);   // before the oldest
    CHECK(Trail::FindBySeq(trail, 13) == nullptr);  // after the newest
    CHECK(Trail::FindBySeq({}, 1) == nullptr);
}

TEST_CASE("thin: within the limit, newer half intact, older half sparse") {
    Config::TrailSettings cfg;
    cfg.max_footprints = 100;
    cfg.thin_spacing   = 256.0f;
    auto trail         = Walk(101, 64.0f);
    const auto newest  = trail;  // copy

    Trail::Thin(trail, cfg);

    CHECK(trail.size() <= 100);
    // seq still increasing
    for (std::size_t i = 1; i < trail.size(); ++i) {
        CHECK(trail[i].seq > trail[i - 1].seq);
    }
    // The newer half (from the last footprint of the older half on) is untouched
    const std::size_t half = 101 / 2;
    for (std::size_t i = half - 1; i < newest.size(); ++i) {
        CHECK(Trail::FindBySeq(trail, newest[i].seq) != nullptr);
    }
    // In the older part kept footprints are at least thin_spacing apart (on one line here)
    const auto boundary = newest[half - 1].seq;
    for (std::size_t i = 1; i < trail.size() && trail[i].seq < boundary; ++i) {
        CHECK(trail[i].pos.GetDistance(trail[i - 1].pos) >= cfg.thin_spacing);
    }
    // The oldest footprint is kept: the trail still starts where it started
    CHECK(trail.front().seq == 1);
}

TEST_CASE("thin: a change of space is always kept") {
    Config::TrailSettings cfg;
    cfg.max_footprints = 16;
    cfg.thin_spacing   = 256.0f;
    auto trail         = Walk(17, 8.0f);  // dense: older half thins to almost nothing
    trail[3].space     = 0x1234;          // stepped into an interior and out
    trail[4].space     = 0x1234;

    Trail::Thin(trail, cfg);

    CHECK(Trail::FindBySeq(trail, 4) != nullptr);  // entering the interior
    CHECK(Trail::FindBySeq(trail, 6) != nullptr);  // back outside
}

TEST_CASE("thin: an already sparse trail over the limit loses its oldest") {
    Config::TrailSettings cfg;
    cfg.max_footprints = 20;
    cfg.thin_spacing   = 256.0f;
    auto trail         = Walk(25, 300.0f);  // nothing to thin: every gap is over thin_spacing

    Trail::Thin(trail, cfg);

    REQUIRE(trail.size() == 20);
    CHECK(trail.front().seq == 6);
    CHECK(trail.back().seq == 25);
}
