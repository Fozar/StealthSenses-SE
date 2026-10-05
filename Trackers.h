#pragma once

// NPC trackers: suspicious NPCs that notice the player's trail walk it footprint by
// footprint. Steering: a runtime XMarker linked to the NPC with LinkCustom02/03 and the
// vanilla defaultTravelToLinkCustom* package. Game-thread only.
namespace StealthSenses::Trackers {
    // Engine state a tracker changes on an NPC; saved so a loaded game can undo it.
    struct Binding {
        RE::FormID actor      = 0;
        RE::FormID marker     = 0;
        RE::FormID prevLinked = 0;  // what the NPC was linked to with our keyword before
    };

    // Looks up the vanilla package, keyword and XMarker; needs data loaded.
    bool Init();

    // Called every Config tracker.interval_ms of unpaused play.
    void Update(float a_deltaSeconds);

    // Forgets trackers without touching the engine (revert/load: actors may be gone).
    void Clear();

    std::vector<Binding> Bindings();

    // Bindings read from the co-save; undone by ReleaseStale once the game is loaded.
    void QueueStale(std::vector<Binding> a_bindings);
    void ReleaseStale();
}
