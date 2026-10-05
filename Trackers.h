#pragma once

// NPC trackers: suspicious NPCs near the player's trail get the next footprint
// as a quiet detection event, one point at a time. Game-thread only.
namespace StealthSenses::Trackers {
    // Called every Config tracker.interval_ms of unpaused play.
    void Update(float a_deltaSeconds);

    void Clear();
}
