# StealthSenses SE

An SKSE plugin for **Skyrim Special Edition / Anniversary Edition** that turns your footprints and scent into evidence. Hostile NPCs who have reason to be suspicious can notice the trail you leave, read it, and follow it — bending down to examine a print, losing it on bare rock, casting around to pick it up again, and eventually walking straight into your hiding place. Creatures with the right scent profile can follow your scent even over stone and in darkness.

**Nexus Mods:** https://www.nexusmods.com/skyrimspecialedition/mods/194163 · **Source:** https://github.com/Fozar/StealthSenses-SE

> **Status: early access (0.12.0).** Visual tracking and wolf scent pursuit have been tested in game on AE 1.7.104. Expect tuning changes; there is no ESP. See `CHANGELOG.md`.

---

## What it does

- **Your trail** — every step you take on foot leaves a footprint. Soft ground holds a clear print, stone barely any; prints fade over game time (faster in rain or snow), water breaks the trail, blood makes it last. The trail is saved with your game.
- **Trackers** — a hostile humanoid with a reason to be suspicious that sees one of your prints in front of it takes up the trail: draws its weapon, follows print to print, bends down to examine one, searches where the trail runs out, and gives up after a while.
- **Different eyes** — not every enemy reads a trail equally well. Bosmer, hunters, the Forsworn, the Alik'r, the Thalmor, the Dark Brotherhood, the Silver Hand and the Dawnguard notice prints from farther away and make out fainter ones. Darkness hides your prints from most enemies, but not from Khajiit and vampires, and a vampire picks out a bloody print from afar. Falmer are blind to them. The profiles are a JSON file that you or other mods can extend.
- **Alarming tracks** — a werewolf's or a vampire lord's prints, or bloody ones, alarm even a calm enemy who sees them.
- **Scent** — selected creatures can investigate your scent trail, including on stone and in darkness. It gradually spreads and weakens with game time; blood strengthens it, and water breaks its chain. Khajiit and Argonians have stronger scent profiles than ordinary people; vampires have an additional advantage on blood. These profiles can be adjusted in JSON.
- **Bodies** — an enemy that finds the body of one of its own you killed goes to it and looks for your trail from there.
- **Sharper senses** — a suspicious enemy (reading visible tracks, after a fight with you, or having found a body) spots you sooner: its stealth meter on you reads a few points higher. Scent pursuit alone adds no detection bonus.
- **Vanilla AI stays in charge** — trackers walk with vanilla packages and pathfinding; one that spots you or fights you (or your followers) is handed back to the vanilla AI at once. A fight with someone else — a wolf on the road — only interrupts the chase.

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

Install the archive with a mod manager, or copy `SKSE\Plugins\StealthSenses.dll`, `SKSE\Plugins\StealthSensesConfig.json` and `SKSE\Plugins\StealthSensesProfiles.json` into `Data\SKSE\Plugins\`. No ESP, no load order requirements.

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

### `step` — how you walk

A footprint's visibility is multiplied by how you stepped when you left it. The multipliers below combine; the result stays within `min_mult`…`max_mult`.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Off = every step counts the same, as before 0.10 |
| `skill_floor` | `0.6` | Multiplier at Sneak 100 (1.0 at 0, in between in proportion) |
| `sneak_mult` | `0.7` | Sneaking |
| `walk_mult` | `0.9` | Walking (running is 1.0) |
| `sprint_mult` | `1.25` | Sprinting |
| `heavy_boots_mult` | `1.15` | Heavy armor boots |
| `heavy_armor_mult` | `1.1` | Heavy armor cuirass |
| `overencumbered_mult` | `1.25` | Over-encumbered |
| `light_foot_mult` | `0.8` | With the Light Foot perk |
| `khajiit_mult` | `0.85` | Khajiit, everywhere |
| `bosmer_mult` | `0.8` | Bosmer, on grass and dirt outdoors |
| `argonian_mult` | `0.8` | Argonian, in mud outdoors |
| `beast_form_mult` | `1.5` | Werewolf, or vampire lord on the ground (no footprints while levitating) |
| `min_mult` / `max_mult` | `0.25` / `2.0` | Bounds of the combined multiplier |
| `khajiit_races`, `argonian_races`, `bosmer_races` | vanilla races and their vampire variants | Races as `"Plugin.esp\|FormID"` (hex, without the load order byte). Add a custom race here to give it that race's step |

You leave no footprints while jumping or falling.

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
| `fight_pause_seconds` | `60` | A tracker fighting someone else (a wolf) goes back to your trail after the fight if it lasted no longer than this (0 = drops the trail) |
| `require_hostile` | `true` | Only NPCs hostile to you track |
| `alarm_beast` | `true` | A werewolf's or a vampire lord's footprint makes even a calm hostile take up the trail |
| `alarm_blood` | `true` | …and so does a bloody one |
| `include_combat` | `false` | Keep tracking NPCs that are in combat (they ignore packages; leave off) |
| `max_trackers` | `4` | Trackers at the same time |
| `min_visibility` | `0.2` | A footprint at least this visible is *clear*: read from afar and across gaps |
| `faint_visibility` | `0.1` | A footprint at least this visible is *faint*: read only up close |
| `close_read_radius` | `300` | "Up close" for faint footprints |
| `max_height_diff` | `200` | Footprints this much higher or lower than the NPC (a ledge, the floor above) are ignored |
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

### `dark` — night hides footprints

Outdoors, darkness follows the hour; indoors it is constant (torches are not taken into account). How much an NPC loses by it depends on its profile (`night`).

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Off: darkness changes nothing |
| `night_penalty` | `0.5` | Share of a footprint's visibility lost in full darkness |
| `interior` | `0.3` | Darkness indoors (0 = daylight, 1 = night) |
| `dusk_hour` / `night_hour` | `18` / `21` | Darkness grows from dusk to night… |
| `dawn_hour` / `day_hour` | `5` / `7` | …and fades from dawn to day |

### `scent` — scent sources

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Enable scent sources and scent pursuit |
| `halflife_hours` | `0.5` | Game hours for scent quantity to halve; concentration also falls from dilution |
| `base_strength` | `1.0` | Initial ordinary scent strength |
| `blood_bonus` | `1.0` | Added strength for bloody sources |
| `radius` | `450.0` | Initial reference scent range in game units, modified by NPC profiles |
| `min_strength` | `0.1` | Minimum perceived scent strength |
| `spread_max_mult` | `1.25` | Maximum expansion of the reference radius; 1 disables expansion |
| `spread_hours` | `0.1` | Game hours for expansion to approach its limit (time constant) |

### Sense profiles — `StealthSensesProfiles.json`

Who reads footprints and how well. A profile has `tracking` (multiplies footprint visibility and the distance a print is noticed from; 0 = never reads them), `night` (how much darkness hurts: 1 = fully, 0 = sees in the dark) and `blood` (an extra multiplier for bloody prints). A rule matches on a `keyword`, `race`, `class`, `faction` or `npc` given as `"Plugin.esp|FormID"`, and rules apply in that order, so a faction overrides a race. `tracking_mult` and `blood_mult` multiply the value set before them instead of replacing it: a Bosmer hunter keeps the Bosmer bonus. Anything no rule matches gets `default`; it is `tracking: 0`, so only NPCs with the vanilla `ActorTypeNPC` keyword read footprints.

Scent uses independent fields: `smell` sets its range multiplier (0 disables scent), `smell_blood` adds a range multiplier for bloody sources, `smell_mult` multiplies earlier scent settings, and `smell_hunt` allows a calm enemy to investigate ordinary scent. These fields follow the same rule order. They do not change vanilla hostility.

Rules can include a `why` field explaining their source. Mods can add `StealthSensesProfiles_<Name>.json` next to it instead of editing it; such files are read after it, in name order.

```json
{ "rules": [ { "faction": "MyMod.esp|000D62", "name": "rangers", "tracking_mult": 1.4 } ] }
```

### `detection` — suspicious NPCs see you sooner

Bonuses are stealth meter points added to what the game computes for that NPC; the largest one that applies counts. An NPC in combat gets none: the vanilla search stays as it is.

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `true` | Off: detection is vanilla |
| `tracker_bonus` | `15` | An NPC on your trail |
| `after_fight_bonus` | `10` | A hostile after a fight with you is over (`after_combat_seconds`) |
| `found_body_bonus` | `10` | An NPC that found the body of one of its own (`body_found_seconds`) |

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
- Works alongside detection overhauls: suspicious NPCs get a bonus on top of whatever the game (and other mods) compute; `detection.enabled: false` turns it off. Mods that change the stealth meter or GMSTs shift how often NPCs become suspicious (`caution_level`).
- Papyrus Extender hooks the same detection call; both run (tested with Papyrus Extender 6.5.2).
- AI overhauls that override NPC packages aggressively may fight the tracker's package; the plugin re-applies it once per second.

## Known limitations

- Trackers do not speak alert lines of their own.
- Scent uses a conservative direct barrier check; spreading around corners is not simulated.
- Scent comes from recorded ground steps; a continuous scent source from a stationary character is not simulated.
- Enhanced scent in a werewolf's human form is not yet detected automatically.

## FAQ

**Who can track me?** Visible footprints are read by NPCs with a nonzero `tracking` profile, normally humanoids with `ActorTypeNPC`. Scent is a separate channel for selected creatures and races with a nonzero `smell` profile. Automatons, ordinary undead, atronachs, gargoyles and other excluded creatures receive no scent sense; vampires and death hounds have explicit exceptions. Dremora and Seekers have no scent profile by default. Both channels can be adjusted in `StealthSensesProfiles.json`.

**Does it work with Footprints, Dynamic Footprints or Dynamic Terrain Deformation?** Yes, alongside them; it replaces nothing. Those mods draw prints you can see; Stealth Senses draws nothing and keeps its own invisible trail for the AI. The two are separate: a print you see is not necessarily one an enemy reads, and on stone, where you see none, a tracker can still make out a faint one up close.

**Special Edition 1.5.97?** The same DLL supports SE 1.5.97 and AE through Address Library, so it should work; it is tested on AE 1.7.104 only. You need SKSE 2.0.20 and the SE version of Address Library. If you try it, please report how it went.

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
