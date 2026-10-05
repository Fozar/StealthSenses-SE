# Changelog

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
