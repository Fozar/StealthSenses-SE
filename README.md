# StealthSenses SE

An SKSE plugin for **Skyrim Special Edition / Anniversary Edition** that turns your footprints into evidence. Hostile NPCs who have reason to be suspicious can notice the trail you leave, read it, and follow it — bending down to examine a print, losing it on bare rock, casting around to pick it up again, and eventually walking straight into your hiding place.

**Nexus Mods:** https://www.nexusmods.com/skyrimspecialedition/mods/194163

> **Status: early access (0.7.x).** The trail mechanic works and is tested in game on AE 1.6.1170. Expect tuning changes; there is no ESP and nothing is baked into your save beyond a small SKSE co-save record. See `CHANGELOG.md`.

---

## How it works

### Your trail

- Every step you take on foot is recorded as a footprint (one per ~64 units of movement), using the game's own footstep events.
- Each footprint remembers **where**, **when** (game time) and **what you stepped on**. Snow, mud, dirt, sand, ash and grass hold a clear print; gravel less so; stone, wood and ice barely at all.
- Footprints **fade over game time** — visibility halves every 2 game hours, three times faster in rain or snow outdoors.
- Water breaks the trail (swimming and wading leave nothing). Riding leaves nothing either.
- When you get hit, the next 20 footprints carry **blood** and stay visible much longer.
- The trail is saved with your game.

How long a footprint stays useful with default settings (1 game hour ≈ 3 real minutes at the default timescale):

| Surface | Readable from a distance | Readable up close |
|---|---|---|
| Snow, mud, dirt, sand, grass | ~4.6 game hours | ~6.6 game hours |
| Gravel | ~3.2 h | ~5.2 h |
| Stone, wood, ice | never | ~1.2 h |
| With blood | about 2 h longer | |

### Who tracks you

An NPC becomes a tracker only when **all** of these hold:

- it is **hostile** to you and a **humanoid** (no rabbits on your trail);
- it is **suspicious**: it lost you in a fight within the last minute, *or* it almost spotted you (its detection of you is high), *or* it is in the vanilla alert state;
- it is standing or walking — not sitting, sleeping or getting into furniture;
- one of your footprints lies **right in front of it**: within ~5 m and inside a 120° view cone.

An NPC that sees you outright, or enters combat, is handed back to the vanilla AI immediately.

### What a tracker does

- **Picks up the trail** — draws its weapon and goes on alert.
- **Follows** the freshest footprint it can read ahead, walking from print to print. Clear prints are read from ~13 m away; faint ones (on stone) only within ~4 m, so on rock it moves slowly and carefully.
- **Examines** a print now and then — bends down to the ground.
- **Loses the trail** where it runs out or crosses bare rock: it stops, looks around, then walks to search points around the last print — first in the direction the trail was heading, then wider — looking around at each.
- **Picks the trail up again** if it finds a continuation (also across short gaps of rock).
- **Gives up** after ~45 seconds without a new footprint and goes back to what it was doing. It will not take up the same old trail again.

Trackers walk with vanilla AI packages and pathfinding; the plugin only decides *where* they go next.

---

## Requirements

- Skyrim **Anniversary Edition 1.6.x** — tested on 1.6.1170.
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
| `max_footprints` | `2048` | Footprints kept in memory (roughly 10–20 minutes of walking); the oldest go first |
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
| `caution_level` | `70` | Detection of you (0–100) at which a hostile counts as suspicious |
| `after_combat_seconds` | `60` | A hostile that fought you stays suspicious this long after the fight |
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

- NPCs can notice a footprint behind a wall or rock if it is in front of them and close.
- Trackers do not speak alert lines of their own.
- Animals never track (planned together with scent).

---

## Building from source

CMake 3.28+, MSVC (Visual Studio 2022 Build Tools), C++23, Ninja. Dependencies (CommonLibSSE-NG v10.1.0, fmt, spdlog, nlohmann_json, DirectXTK headers) are fetched on configure; CommonLibSSE compiles from source on the first build (~15 minutes; limit parallelism to `-j 4` on 16 GB RAM).

Configure presets (`CMakePresets.json`, selectable as profiles in CLion):

- **`release`** → `cmake-build-release/StealthSenses.zip`
- **`dev`** → `cmake-build-dev/`, with test switches compiled in (console echo of tracker decisions, `debug_all_npcs` to turn every NPC into a tracker, footstep tag logging)

To copy the DLL into a mod manager folder after every build, set `STEALTHSENSES_DEPLOY_DIR`, e.g. in a local `CMakeUserPresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "dev-mo2", "inherits": "dev",
      "cacheVariables": { "STEALTHSENSES_DEPLOY_DIR": "D:/Modding/MO2/mods/StealthSenses/SKSE/Plugins" } }
  ]
}
```

```
cmake --preset release
cmake --build cmake-build-release -j 4
```
