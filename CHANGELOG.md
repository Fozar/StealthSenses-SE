# Changelog

## 0.7.2 — 2026-10-06

- **Walls hide footprints** — an enemy notices a footprint only if it can actually see it: not behind a wall, a rock or a closed door. New option `tracker.notice_line_of_sight` (on by default).
- **Longer memory for your trail** — footprints that have faded beyond reading are forgotten right away instead of an hour later, and when the trail reaches `max_footprints` its older half is thinned out (one footprint per `trail.thin_spacing` units, 256 by default) instead of being cut off. Long trails stay followable for as long as they are visible.
- **Safer on Special Edition** — the tracker's alert state is now set through a CommonLibSSE field instead of an engine function looked up by address (the function does exactly that, checked in the 1.7.104 executable). Its Special Edition address was only matched by name; every address the plugin uses now comes from CommonLibSSE.
- **Correction** — 0.7.0 and 0.7.1 were tested on Anniversary Edition **1.7.104**, not 1.6.1170 as stated before.

## 0.7.1 — 2026-10-06

No gameplay changes.

- **License** — Stealth Senses is now released under the GNU GPL v3.0 (or later), as required by CommonLibSSE-NG it is built on. The license text is included as `LICENSE.txt`; the full source code is at https://github.com/Fozar/StealthSenses-SE.

## 0.7.0 — 2026-10-06

First public version (early access). Footprints as evidence for the AI:

- **Your trail** — every step you take on foot is recorded with the surface you stepped on. Snow, mud, dirt, sand and grass hold a clear print, gravel less so, stone/wood/ice barely at all. Footprints fade over game time (faster in rain and snow), water breaks the trail, being hit leaves a blood trail. The trail is saved with your game.
- **Trackers** — a hostile humanoid that has a reason to be suspicious (lost you in a fight within the last minute, almost spotted you, or is alerted) and sees one of your footprints right in front of it takes up the trail: draws its weapon, goes on alert, follows the prints, bends down to examine one now and then, slows down on faint prints on rock.
- **Searching** — where the trail runs out the tracker stops and looks around, then searches points around the last print, first in the direction you were heading, then wider; it picks the trail up again if it finds a continuation, and gives up after ~45 seconds.
- **Vanilla AI stays in charge** — trackers walk with vanilla packages and pathfinding; an NPC that spots you or enters combat is handed back to the vanilla AI immediately. No ESP, no scripts, no edited records.
- **Safe to remove** — the plugin undoes its changes to a tracking NPC when a save is loaded.
- **Bug reports** — optional telemetry (`debug.telemetry`) plus a mark key (F7) to point at the moment something looked wrong.

Tested on Anniversary Edition 1.6.1170. Special Edition 1.5.97 is expected to work (Address Library) but has not been tested.

Known limitations: NPCs can notice a footprint behind a wall if it is right in front of them; trackers do not speak alert lines of their own; animals never track.
