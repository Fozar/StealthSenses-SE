#pragma once

namespace StealthSenses::Config {
    struct TrailSettings {
        bool  enabled                = true;
        float min_spacing            = 64.0f;   // units between two recorded footprints
        int   max_footprints         = 2048;
        int   poll_interval_ms       = 250;     // fallback sampler when no footstep events arrive
        float halflife_hours         = 2.0f;    // game hours until visibility halves
        float weather_halflife_mult  = 0.35f;   // halflife multiplier in rain/snow (exterior only)
        float soft_visibility        = 1.0f;    // snow, mud, dirt, sand, ash, grass
        float gravel_visibility      = 0.6f;
        float hard_visibility        = 0.15f;   // stone, wood, ice, carpet, everything else
        float unknown_visibility     = 0.5f;    // MATERIAL_ID::kNone
        int   blood_steps            = 20;      // footprints flagged as bloody after the player is hit
        float blood_visibility_bonus = 1.0f;
    };

    struct TrackerSettings {
        bool        enabled          = true;
        int         interval_ms      = 1000;
        float       notice_radius    = 350.0f;  // an NPC notices a footprint only this close
        float       lead_distance    = 900.0f;  // next footprint must be this close to the NPC
        float       gap_distance     = 1200.0f; // look this far past the last point when the trail breaks
        float       arrive_radius    = 300.0f;  // travel packages stop ~260-280 short of the marker
        float       retarget_seconds = 12.0f;   // give the next point even if the NPC never arrived
        float       lost_seconds     = 45.0f;   // drop the tracker after this long without a new footprint
        float       examine_seconds  = 3.5f;    // stands bent over a footprint (humanoids)
        float       examine_every    = 15.0f;   // at most one examine per this many seconds
        float       search_radius    = 700.0f;  // search hops land 250..this from the last footprint
        float       hop_timeout      = 10.0f;   // give up walking to a search point after this
        float       look_seconds     = 4.0f;    // looks around at each search point
        bool        draw_weapon      = true;    // hostile humanoid trackers draw their weapon
        float       repickup_seconds = 15.0f;   // cooldown after a tracker is dropped
        float       min_visibility   = 0.2f;
        int         caution_level    = 40;      // stealth meter value 0..100 that counts as "suspicious"
        bool        require_hostile  = false;
        bool        include_combat   = false;
        int         max_trackers     = 4;
        bool        debug_all_npcs   = false;   // every High-process NPC tracks (AI obedience test)
    };

    struct DebugSettings {
        std::string log_level         = "info";
        bool        console           = true;   // echo tracker decisions to the in-game console
        bool        log_footstep_tags = true;
        bool        telemetry         = true;   // StealthSenses.trace.jsonl next to the log
        int         mark_key          = 0x41;   // DirectInput scan code; 0x41 = F7, 0 = off
    };

    struct Settings {
        TrailSettings   trail;
        TrackerSettings tracker;
        DebugSettings   debug;
    };

    const Settings& Get();
    void Load();
}
