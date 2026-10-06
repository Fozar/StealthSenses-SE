// Stealth Senses — SKSE plugin for Skyrim SE/AE
// Copyright (C) 2026 fozar
//
// This program is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Dev build (CMake STEALTHSENSES_DEV=ON, preset "dev"): test switches that must not ship.
// In a release build each dev-only setting is a static constexpr false — not read from the
// JSON, and the code behind it is compiled out — so call sites need no #ifdef.
#ifdef STEALTHSENSES_DEV
#    define STEALTHSENSES_DEV_SETTING(type, name, value, comment) type name = value;
inline constexpr bool kDevBuild = true;
#else
#    define STEALTHSENSES_DEV_SETTING(type, name, value, comment) static constexpr type name = false;
inline constexpr bool kDevBuild = false;
#endif

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
        float       notice_fov       = 120.0f;  // ...and within this view cone (degrees, full width)
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
        bool        set_alert        = true;    // ...and go into vanilla alert (Actor.SetAlert): alert voice lines
        float       stuck_seconds    = 6.0f;    // not moving this long while walking = unreachable point
        float       stuck_give_up    = 20.0f;   // total such time since the last footprint = drop
        float       repickup_seconds = 15.0f;   // cooldown after a tracker is dropped
        float       min_visibility   = 0.2f;    // clear footprint: read from lead_distance / across gaps
        float       faint_visibility = 0.1f;    // faint footprint (stone 0.15): read only up close
        float       close_read_radius = 300.0f; // "up close" for faint footprints
        int         caution_level    = 70;      // stealth meter 0..100 that counts as "suspicious" (baseline near the player is 41-67)
        float       after_combat_seconds = 60.0f; // a hostile that fought the player stays suspicious this long after
        bool        require_hostile  = true;
        bool        humanoids_only   = true;    // only ActorTypeNPC track (no rabbits on the trail)
        bool        include_combat   = false;
        int         max_trackers     = 4;
        STEALTHSENSES_DEV_SETTING(bool, debug_all_npcs, false, "every High-process NPC tracks (AI obedience test)")
    };

    // Bug-report tools (log level, telemetry, mark key) ship in release too; telemetry is off by
    // default there and a user turns it on to send a trace.
    struct DebugSettings {
        std::string log_level         = "info";
        bool        telemetry         = kDevBuild;  // StealthSenses.trace.jsonl next to the log
        int         telemetry_max_mb  = 50;         // the trace is rotated to .1 beyond this
        int         mark_key          = 0x41;       // DirectInput scan code; 0x41 = F7, 0 = off
        STEALTHSENSES_DEV_SETTING(bool, console, true, "echo tracker decisions to the in-game console")
        STEALTHSENSES_DEV_SETTING(bool, log_footstep_tags, true, "log each new footstep tag with its thread")
    };

    struct Settings {
        TrailSettings   trail;
        TrackerSettings tracker;
        DebugSettings   debug;
    };

    const Settings& Get();
    void Load();
}
