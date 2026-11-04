// SPDX-License-Identifier: GPL-3.0-or-later

#include "CoSaveFormat.h"

#include <doctest/doctest.h>

using namespace StealthSenses;

namespace {
    // Stands in for SKSE::SerializationInterface: one record as a byte buffer. ReadRecordData
    // returns how many bytes it could read, as SKSE does at the end of a record.
    struct FakeCoSave {
        mutable std::vector<std::uint8_t> bytes;
        mutable std::size_t               read = 0;
        // Load order change: old FormID -> new one; nullopt = the form is gone
        std::function<std::optional<std::uint32_t>(std::uint32_t)> resolve = [](std::uint32_t a_id) {
            return std::optional{ a_id };
        };

        template <class T>
        bool WriteRecordData(const T& a_value) const {
            const auto* p = reinterpret_cast<const std::uint8_t*>(std::addressof(a_value));
            bytes.insert(bytes.end(), p, p + sizeof(T));
            return true;
        }

        template <class T>
        std::uint32_t ReadRecordData(T& a_value) const {
            const auto n = std::min(sizeof(T), bytes.size() - read);
            std::memcpy(std::addressof(a_value), bytes.data() + read, n);
            read += n;
            return static_cast<std::uint32_t>(n);
        }

        bool ResolveFormID(std::uint32_t a_old, std::uint32_t& a_new) const {
            const auto id = resolve(a_old);
            if (id) {
                a_new = *id;
            }
            return id.has_value();
        }
    };

    Trail::Footprint Footprint(float a_x, RE::FormID a_space, float a_hours, RE::MATERIAL_ID a_material, std::uint8_t a_flags) {
        Trail::Footprint fp;
        fp.pos       = { a_x, a_x + 1.0f, a_x + 2.0f };
        fp.space     = a_space;
        fp.gameHours = a_hours;
        fp.material  = a_material;
        fp.flags     = a_flags;
        fp.seq       = 77;  // not saved
        return fp;
    }
}

TEST_CASE("trail record: round trip keeps every saved field") {
    std::deque<Trail::Footprint> trail{
        Footprint(10.5f, 0x0000003C, 12.25f, RE::MATERIAL_ID::kSnow, Trail::kNone),
        Footprint(-300.0f, 0x000133C6, 13.5f, RE::MATERIAL_ID::kStone, Trail::kBlood),
    };
    trail[1].step = 0.42f;
    trail[0].scentSegment = 7;
    trail[1].scentSegment = 9;
    trail[1].scentBody = 1.05f;
    FakeCoSave co;
    CoSave::WriteTrail(co, trail);
    const auto read = CoSave::ReadTrail(co);

    REQUIRE(read.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(read[i].pos == trail[i].pos);
        CHECK(read[i].space == trail[i].space);
        CHECK(read[i].gameHours == trail[i].gameHours);
        CHECK(read[i].material == trail[i].material);
        CHECK(read[i].flags == trail[i].flags);
        CHECK(read[i].step == trail[i].step);
        CHECK(read[i].scentSegment == trail[i].scentSegment);
        CHECK(read[i].scentBody == trail[i].scentBody);
    }
}

TEST_CASE("trail record: the on-disk layout does not change") {
    // v4 appends the body snapshot to the v3 prefix.
    CHECK(CoSave::kTrailVersion == 4);
    FakeCoSave co;
    auto       fp = Footprint(1.0f, 0x3C, 2.0f, RE::MATERIAL_ID::kDirt, Trail::kBlood);
    fp.step       = 0.75f;
    fp.scentSegment = 17;
    CoSave::WriteTrail(co, { fp });
    REQUIRE(co.bytes.size() == 4 + 37);
    std::uint32_t count, space, material, segment;
    float         x, hours, step;
    std::memcpy(&count, co.bytes.data(), 4);
    std::memcpy(&x, co.bytes.data() + 4, 4);
    std::memcpy(&space, co.bytes.data() + 16, 4);
    std::memcpy(&hours, co.bytes.data() + 20, 4);
    std::memcpy(&material, co.bytes.data() + 24, 4);
    std::memcpy(&step, co.bytes.data() + 29, 4);
    std::memcpy(&segment, co.bytes.data() + 33, 4);
    CHECK(count == 1);
    CHECK(x == 1.0f);
    CHECK(space == 0x3C);
    CHECK(hours == 2.0f);
    CHECK(material == static_cast<std::uint32_t>(RE::MATERIAL_ID::kDirt));
    CHECK(co.bytes[28] == Trail::kBlood);
    CHECK(step == 0.75f);
    CHECK(segment == 17);
}

TEST_CASE("trail record: v1 from saves before 0.10 reads with step 1") {
    // v1: count u32, then 25-byte {x y z f32, space u32, hours f32, material u32, flags u8}
    FakeCoSave co;
    co.WriteRecordData(std::uint32_t{ 2 });
    co.WriteRecordData(CoSave::SavedFootprint{ 1.0f, 2.0f, 3.0f, 0x3C, 4.0f, static_cast<std::uint32_t>(RE::MATERIAL_ID::kSnow), Trail::kBlood });
    co.WriteRecordData(CoSave::SavedFootprint{ 5.0f, 6.0f, 7.0f, 0x3C, 8.0f, static_cast<std::uint32_t>(RE::MATERIAL_ID::kDirt), 0 });
    const auto read = CoSave::ReadTrail(co, 1);
    REQUIRE(read.size() == 2);
    CHECK(read[0].pos == RE::NiPoint3{ 1.0f, 2.0f, 3.0f });
    CHECK(read[0].flags == Trail::kBlood);
    CHECK(read[0].step == 1.0f);
    CHECK(read[1].gameHours == 8.0f);
    CHECK(read[1].step == 1.0f);
    CHECK(read[0].scentBody == 1.0f);
}

TEST_CASE("trail record: a broken step reads as 1") {
    FakeCoSave co;
    co.WriteRecordData(std::uint32_t{ 1 });
    co.WriteRecordData(CoSave::SavedFootprintV2{ { 1.0f, 2.0f, 3.0f, 0x3C, 4.0f, 0, 0 }, std::numeric_limits<float>::quiet_NaN() });
    const auto read = CoSave::ReadTrail(co, 2);
    REQUIRE(read.size() == 1);
    CHECK(read[0].step == 1.0f);
    CHECK(read[0].scentSegment == 0);
    CHECK(read[0].scentBody == 1.0f);
}

TEST_CASE("trail record: v3 defaults body and v4 rejects malformed body snapshots") {
    FakeCoSave legacy;
    legacy.WriteRecordData(std::uint32_t{1});
    legacy.WriteRecordData(CoSave::SavedFootprintV3{{{1, 2, 3, 0x3C, 4, 0, 0}, 0.5f}, 17});
    const auto old = CoSave::ReadTrail(legacy, 3);
    REQUIRE(old.size() == 1);
    CHECK(old[0].scentSegment == 17);
    CHECK(old[0].scentBody == 1);
    for (float bad : { -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() }) {
        FakeCoSave co;
        auto fp = Footprint(1, 0x3C, 4, RE::MATERIAL_ID::kDirt, 0);
        fp.scentBody = bad;
        CoSave::WriteTrail(co, {fp});
        const auto read = CoSave::ReadTrail(co);
        REQUIRE(read.size() == 1);
        CHECK(read[0].scentBody == 1);
    }
}

TEST_CASE("trail record: v2 keeps stride without inventing legacy scent or water continuity") {
    FakeCoSave co;
    co.WriteRecordData(std::uint32_t{ 1 });
    co.WriteRecordData(CoSave::SavedFootprintV2{ { 1, 2, 3, 0x3C, 4, 0, Trail::kBlood }, 0.42f });
    const auto read = CoSave::ReadTrail(co, 2);
    REQUIRE(read.size() == 1);
    CHECK(read[0].step == doctest::Approx(0.42f));
    CHECK(read[0].flags == Trail::kBlood);
    CHECK(read[0].scentSegment == 0);
    CHECK(read[0].scentBody == 1.0f);
}

TEST_CASE("trail record: unknown future layout is not silently read as an older format") {
    FakeCoSave co;
    CoSave::WriteTrail(co, { Footprint(1, 0x3C, 0, RE::MATERIAL_ID::kDirt, 0) });
    CHECK(CoSave::ReadTrail(co, 99).empty());
    CHECK(co.read == 0);
}

TEST_CASE("trail record: load order change remaps spaces and drops gone ones") {
    FakeCoSave co;
    CoSave::WriteTrail(co, { Footprint(1, 0x01000ABC, 0, RE::MATERIAL_ID::kDirt, 0), Footprint(2, 0x02000DEF, 0, RE::MATERIAL_ID::kDirt, 0),
                               Footprint(3, 0x0000003C, 0, RE::MATERIAL_ID::kDirt, 0) });
    co.resolve = [](std::uint32_t a_id) -> std::optional<std::uint32_t> {
        if (a_id == 0x01000ABC) {
            return 0x05000ABC;  // that mod moved in the load order
        }
        if (a_id == 0x02000DEF) {
            return std::nullopt;  // that mod was removed
        }
        return a_id;
    };
    const auto read = CoSave::ReadTrail(co);
    REQUIRE(read.size() == 2);
    CHECK(read[0].space == 0x05000ABC);
    CHECK(read[1].space == 0x3C);
}

TEST_CASE("trail record: a truncated record gives what was read") {
    FakeCoSave co;
    CoSave::WriteTrail(co, { Footprint(1, 0x3C, 0, RE::MATERIAL_ID::kDirt, 0), Footprint(2, 0x3C, 0, RE::MATERIAL_ID::kDirt, 0) });
    co.bytes.resize(co.bytes.size() - 10);
    CHECK(CoSave::ReadTrail(co).size() == 1);

    FakeCoSave empty;
    CHECK(CoSave::ReadTrail(empty).empty());
    FakeCoSave bodyCut;
    CoSave::WriteTrail(bodyCut, { Footprint(1, 0x3C, 0, RE::MATERIAL_ID::kDirt, 0) });
    bodyCut.bytes.pop_back();
    CHECK(CoSave::ReadTrail(bodyCut).empty());
}

TEST_CASE("binding record: round trip, unresolved actor dropped, unresolved marker zeroed") {
    FakeCoSave co;
    CoSave::WriteBindings(co, { { 0x00012345, 0xFF000801, 0x00054321 }, { 0x00022222, 0xFF000802, 0 },
                                  { 0x00033333, 0xFF000803, 0 } });
    co.resolve = [](std::uint32_t a_id) -> std::optional<std::uint32_t> {
        if (a_id == 0x00033333 || a_id == 0xFF000802) {
            return std::nullopt;
        }
        return a_id;
    };
    const auto read = CoSave::ReadBindings(co);
    REQUIRE(read.size() == 2);
    CHECK(read[0].actor == 0x00012345);
    CHECK(read[0].marker == 0xFF000801);
    CHECK(read[0].prevLinked == 0x00054321);
    CHECK(read[1].actor == 0x00022222);
    CHECK(read[1].marker == 0);  // the marker is gone: only its cleanup is skipped
    CHECK(read[1].prevLinked == 0);
}

TEST_CASE("body record: round trip and resolution") {
    CoSave::Bodies bodies;
    bodies.bodies = { { 0x0001EBF6, 18.5f }, { 0x0001EBF0, 19.25f } };
    bodies.seen   = { { 0x0001EBA1, 0x0001EBF6 }, { 0x00029613, 0x0001EBF0 } };
    FakeCoSave co;
    CoSave::WriteBodies(co, bodies);
    co.resolve = [](std::uint32_t a_id) -> std::optional<std::uint32_t> {
        if (a_id == 0x00029613) {
            return std::nullopt;
        }
        return a_id;
    };
    const auto read = CoSave::ReadBodies(co);
    REQUIRE(read.bodies.size() == 2);
    CHECK(read.bodies[0].corpse == 0x0001EBF6);
    CHECK(read.bodies[0].diedAt == 18.5f);
    CHECK(read.bodies[1].diedAt == 19.25f);
    REQUIRE(read.seen.size() == 1);
    CHECK(read.seen[0] == Trackers::BodySeen{ 0x0001EBA1, 0x0001EBF6 });
}

TEST_CASE("body record: cut inside the reactions keeps the bodies") {
    CoSave::Bodies bodies;
    bodies.bodies = { { 0x0001EBF6, 18.5f } };
    bodies.seen   = { { 0x0001EBA1, 0x0001EBF6 }, { 0x0001EBA2, 0x0001EBF6 } };
    FakeCoSave co;
    CoSave::WriteBodies(co, bodies);
    co.bytes.resize(co.bytes.size() - 4);
    const auto read = CoSave::ReadBodies(co);
    CHECK(read.bodies.size() == 1);
    CHECK(read.seen.size() == 1);
}
