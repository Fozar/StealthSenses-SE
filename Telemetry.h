#pragma once

#include <nlohmann/json.hpp>

// Structured trace for offline analysis: StealthSenses.trace.jsonl next to the log, one JSON
// object per line. Record types: player, npc, footprint, log, mark (see CLAUDE.md).
// Game-thread only.
namespace StealthSenses::Telemetry {
    void Open();
    bool Enabled();

    // Adds "t" (real seconds since Open) and "gh" (game hours) and writes the line.
    void Write(nlohmann::json a_record);

    // Human-readable tracker event, mirrored from the log
    void Log(std::string_view a_text);

    // Player pressed the mark key: numbered mark with the player's state
    void Mark();

    nlohmann::json Vec(const RE::NiPoint3& a_pos);
    std::string    Hex(std::uint32_t a_value);
}
