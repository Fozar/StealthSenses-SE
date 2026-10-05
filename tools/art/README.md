# Art for the mod page

Both images are SVG drawn by a small script and rendered to PNG with headless Edge/Chrome.

- `cover.html` — the cover (1920×1080). Self-contained; open it or render it:
  `msedge --headless=new --hide-scrollbars --window-size=1920,1080 --virtual-time-budget=8000 --screenshot=cover.png file:///…/cover.html`
- `banner.html` — Nexus header banner, 1300×372 (render with `--window-size=1300,372`; `--force-device-scale-factor=2` for 2600×744).
- `chase-map.template.html` — top-down map of a real chase. Replace `/*DATA*/` with a JSON object
  `{ player: [{t,x,y,sneak}], bandit: [{t,x,y,combat,mode,seq,tx,ty}], logs: [{t,text}], footprints: [{seq,x,y,base,mat,t}] }`
  cut from `StealthSenses.trace.jsonl` (player / npc / log / footprint records of one chase), then render the same way.
  The numbered events are listed in the script (`events`) with their trace times.

Fonts (Cinzel, Cormorant Garamond, Inter) come from Google Fonts; without network the pages fall back to Georgia / Segoe UI.
Rendered results: `docs/media/cover.png`, `docs/media/banner-1300x372.png` (+ `-2600x744`), `docs/media/chase-map.png`.
