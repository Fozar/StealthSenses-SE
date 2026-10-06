# StealthSenses SE

An SKSE plugin for **Skyrim Special Edition / Anniversary Edition** that turns your footprints into evidence. Hostile NPCs who have reason to be suspicious can notice the trail you leave, read it, and follow it — bending down to examine a print, losing it on bare rock, casting around to pick it up again, and eventually walking straight into your hiding place.

**Nexus Mods:** https://www.nexusmods.com/skyrimspecialedition/mods/194163 · **Source:** https://github.com/Fozar/StealthSenses-SE

> **Status: early access (0.8.x).** The trail mechanic works and is tested in game on AE 1.7.104. Expect tuning changes; there is no ESP and nothing is baked into your save beyond a small SKSE co-save record. See `CHANGELOG.md`.

---

## What it does

- **Your trail** — every step you take on foot leaves a footprint. Soft ground holds a clear print, stone barely any; prints fade over game time (faster in rain or snow), water breaks the trail, blood makes it last. The trail is saved with your game.
- **Trackers** — a hostile humanoid with a reason to be suspicious that sees one of your prints in front of it takes up the trail: draws its weapon, follows print to print, bends down to examine one, searches where the trail runs out, and gives up after a while.
- **Bodies** — an enemy that finds the body of one of its own you killed goes to it and looks for your trail from there.
- **Vanilla AI stays in charge** — trackers walk with vanilla packages and pathfinding; one that spots you or starts a fight is handed back to the vanilla AI at once.

The full description is on the [Nexus page](https://www.nexusmods.com/skyrimspecialedition/mods/194163).

---

## Requirements

- Skyrim **Anniversary Edition 1.6.x and 1.7.x** — tested on 1.7.104; 1.6.x is expected to work but untested.
- Skyrim Special Edition 1.5.97 — expected to work (Address Library based), **not tested**.
- [SKSE64](https://skse.silverlock.org/) matching your runtime
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)

Skyrim VR is not supported.

---

## Installation

Install the archive with a mod manager, or copy `SKSE\Plugins\StealthSenses.dll` and `SKSE\Plugins\StealthSensesConfig.json` into `Data\SKSE\Plugins\`. No ESP, no load order requirements.

**Uninstalling** mid-playthrough is safe: the plugin keeps its data in the SKSE co-save. If you save while an NPC is tracking you, the plugin undoes its changes to that NPC the next time the save is loaded — so load the save once with the plugin still installed before removing it.

---

## Configuration

`Data\SKSE\Plugins\StealthSensesConfig.json`. Every key is optional; a missing key or a value of the wrong type keeps the default. Distances are in game units (~70 units ≈ 1 m), times in real seconds unless noted.

### `trail` — your footprints

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Record footprints at all |
| `min_spacing` | `64` | Distance between two recorded footprints |
| `max_footprints` | `2048` | Footprints kept in memory; beyond that the older half of the trail is thinned out |
| `thin_spacing` | `256` | Distance between footprints kept in a thinned-out part of the trail |
| `poll_interval_ms` | `250` | Fallback position sampling when no footstep events arrive |
| `halflife_hours` | `2.0` | Game hours until a footprint is half as visible |
| `weather_halflife_mult` | `0.35` | Half-life multiplier in rain or snow (exteriors) |
| `soft_visibility` | `1.0` | Snow, mud, dirt, sand, ash, grass |
| `gravel_visibility` | `0.6` | Gravel |
| `hard_visibility` | `0.15` | Stone, wood, ice and everything else |
| `unknown_visibility` | `0.5` | Surface the game reports no material for |
| `blood_steps` | `20` | Footprints marked as bloody after you are hit |
| `blood_visibility_bonus` | `1.0` | Added to a bloody footprint's visibility |

### `tracker` — NPCs following the trail

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | NPCs track at all |
| `interval_ms` | `1000` | How often trackers are updated |
| `notice_radius` | `350` | An NPC notices a footprint only this close… |
| `notice_fov` | `120` | …and inside this view cone (degrees, full width) |
| `notice_line_of_sight` | `true` | …and not hidden behind a wall or a rock |
| `caution_level` | `70` | Detection of you (0–100) at which a hostile counts as suspicious |
| `after_combat_seconds` | `60` | A hostile that fought you stays suspicious this long after the fight |
| `body_found_seconds` | `120` | A hostile that found the body of one of its own you killed stays suspicious this long (0 = off) |
| `body_notice_radius` | `1000` | …if it saw the body this close (in its view cone, not behind a wall) |
| `body_know_radius` | `3000` | A hostile that only heard of the body (vanilla noticed the death) goes to it from this close |
| `body_max_age_hours` | `24` | Bodies older than this (game hours) are no longer news |
| `give_up_seconds` | `240` | A tracker gives up after this long on the trail, however fresh it is |
| `leash_distance` | `6000` | …or once this far from where it started |
| `tired_cooldown` | `180` | After giving up like that it takes up no trail for this long |
| `require_hostile` | `true` | Only NPCs hostile to you track |
| `humanoids_only` | `true` | Only humanoids track (no animals) |
| `include_combat` | `false` | Keep tracking NPCs that are in combat (they ignore packages; leave off) |
| `max_trackers` | `4` | Trackers at the same time |
| `min_visibility` | `0.2` | A footprint at least this visible is *clear*: read from afar and across gaps |
| `faint_visibility` | `0.1` | A footprint at least this visible is *faint*: read only up close |
| `close_read_radius` | `300` | "Up close" for faint footprints |
| `lead_distance` | `900` | How far ahead a tracker reads clear footprints |
| `gap_distance` | `1200` | How far past the last footprint it looks when the trail breaks |
| `arrive_radius` | `300` | Distance at which a footprint or search point counts as reached |
| `retarget_seconds` | `12` | Move on to the next footprint even if this one was not reached |
| `lost_seconds` | `45` | Give up after this long without a new footprint |
| `examine_seconds` | `3.5` | Time spent bent over a footprint |
| `examine_every` | `15` | At most one examination per this many seconds |
| `search_radius` | `700` | Search points land up to this far from the last footprint |
| `hop_timeout` | `10` | Give up walking to a search point after this long |
| `look_seconds` | `4` | Time spent looking around at a search point |
| `draw_weapon` | `true` | Hostile trackers draw their weapon |
| `set_alert` | `true` | Hostile trackers go into the vanilla alert state |
| `stuck_seconds` | `6` | A tracker that does not move this long is sent elsewhere |
| `stuck_give_up` | `20` | …and given up after this much standing still in total |
| `repickup_seconds` | `15` | After giving up, an NPC ignores trails for this long |

### `debug` — logging and bug reports

| Key | Default | Meaning |
|---|---|---|
| `log_level` | `"info"` | `trace`, `debug`, `info`, `warn`, `error` |
| `telemetry` | `false` | Write a detailed trace for bug reports (see below) |
| `telemetry_max_mb` | `50` | Trace size before it is rotated |
| `mark_key` | `65` | Key that drops a numbered mark (DirectInput scan code; 65 = F7, 0 = off) |

---

## Reporting a bug

1. Set `"telemetry": true` in the `debug` section of the config.
2. Play until the odd behavior happens. **Press F7** at that moment — a notification "StealthSenses: Mark 1" confirms it.
3. Send the files from `Documents\My Games\Skyrim Special Edition\SKSE\` (`Skyrim.INI` instead of `Skyrim Special Edition` for some setups):
   - `StealthSenses.log`
   - `StealthSenses.trace.jsonl` (and `StealthSenses.trace.jsonl.prev` — the previous game run)
4. Say what you saw and at which mark ("at mark 2 the bandit walked into a wall").

The trace records your position, every nearby NPC's state and every decision the plugin makes, once per second. It contains no personal data beyond your in-game character's movements.

`tools/replay.html` (in the source repository) opens a trace in any browser: a top-down map of your trail, your path, tracker paths and targets, marks, and a time slider.

---

## Compatibility

- No ESP, no edited records, no scripts. Trackers use vanilla packages from `Skyrim.esm` and a temporary marker placed at runtime.
- Works alongside detection overhauls; it does not change detection itself. Mods that change the stealth meter or GMSTs shift how often NPCs become suspicious (`caution_level`).
- AI overhauls that override NPC packages aggressively may fight the tracker's package; the plugin re-applies it once per second.

## Known limitations

- Trackers do not speak alert lines of their own.
- Animals never track (planned together with scent).

---

## Building from source

CMake 3.28+, MSVC (Visual Studio 2022 Build Tools), C++23, Ninja. Dependencies are fetched on configure.

```
cmake --preset release
cmake --build cmake-build-release -j 4
```

The plugin and `cmake-build-release/StealthSenses.zip` are the result.

---

## License

Copyright (C) 2026 fozar.

Stealth Senses is free software, licensed under the **GNU General Public License v3.0 or later** — see [`LICENSE.txt`](LICENSE.txt). It links against [CommonLibSSE-NG](https://github.com/alandtse/CommonLibVR) (GPL-3.0), so the whole plugin is distributed under the same terms: you may use, modify and redistribute it, provided that derived works are also released under GPL-3.0 with their source code.

This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
