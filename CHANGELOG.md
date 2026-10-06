# Changelog

## 0.8.0 — 2026-10-07

- **Bodies** — an enemy that finds the body of one of its own you killed (or your follower or summon did) walks over to it, looks around, searches the area and takes up your trail if it is there. "Finds" means it saw the body — close, in front of it, not behind a wall — or the game already let it know of the death (it heard the kill) and its vanilla search is over. "One of its own" means a shared faction: bandit and bandit, a guard and a citizen of the same hold. A dead mudcrab, a comrade killed by wolves, a body older than a day or one far across the dungeon are ignored, and every NPC reacts to every body only once — also after loading a save. New options: `body_found_seconds`, `body_notice_radius`, `body_know_radius`, `body_max_age_hours`.
- **Trackers get tired** — after 4 minutes on your trail, or 6000 units (~85 m) from where they started, trackers give up however fresh your prints are, and take up no trail for 3 minutes. Before, a tracker kept following as long as new prints kept appearing. New options: `give_up_seconds`, `leash_distance`, `tired_cooldown`.
- **Fixes** — a runtime marker could stay in the world (and the save) if a tracking NPC was deleted mid-chase; the plugin's per-NPC memory no longer grows for the whole session.
- **Save data** — a new co-save record keeps the bodies of your victims and who already reacted to them. Saves from 0.7.x load fine.

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
