#pragma once

// Player trail: footprints recorded on the game thread, read by Trackers.
// Everything here is game-thread only — no locks.
namespace StealthSenses::Trail {
    enum Flags : std::uint8_t {
        kNone  = 0,
        kBlood = 1 << 0,
    };

    struct Footprint {
        RE::NiPoint3     pos;
        RE::FormID       space    = 0;     // interior cell FormID, or worldspace FormID outside
        float            gameHours = 0.0f; // Calendar::GetHoursPassed() at the time of the step
        RE::MATERIAL_ID  material = RE::MATERIAL_ID::kNone;
        std::uint8_t     flags    = kNone;
        std::uint32_t    seq      = 0;     // monotonically increasing along the trail
    };

    // Space id of a reference: interior cell or worldspace FormID; 0 if not in the world.
    RE::FormID SpaceOf(const RE::TESObjectREFR* a_ref);

    // Records a footprint at the player's position if far enough from the previous one.
    // a_source is only for logging ("footstep" / "poll").
    void TryRecord(RE::PlayerCharacter* a_player, std::string_view a_source);

    // Called from TESHitEvent: the next N footprints carry blood.
    void MarkPlayerHit();

    // Visibility in [0, ~2] at the current game time and weather.
    float Visibility(const Footprint& a_fp, float a_nowHours, bool a_badWeather);

    // Drops footprints that have faded completely.
    void Prune(float a_nowHours);

    const std::deque<Footprint>& Footprints();
    void Clear();

    // For serialization: append in order, assigns fresh seq values.
    void Restore(const Footprint& a_fp);

    // True if a player footstep event arrived within the last second; the poll sampler
    // only records while footstep events are silent.
    bool FootstepEventsActive();
    void NoteFootstepEvent();
}
