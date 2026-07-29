# DockX -- Concept

## The problem (proven demand)
OBS docks live in rows: side columns hold ONE stack, and everything else sits under the
preview. The layout streamers keep asking for (years-old open request on the OBS ideas
board): a full height chat column PLUS a second column of stacked panels beside it. OBS
cannot do it. No shipped plugin does it. The Qt framework OBS is built on supports it via
"dock nesting"; OBS simply leaves the switch off.

## The product
A native OBS plugin that flips that switch and then productizes layout freedom:

**v0.1 (prototype, built 2026-07-29):** enable dock nesting + Tools > DockX status dialog.
Goal: prove nested drag-and-drop works and survives OBS restarts (Qt saves/restores
nested layouts through the same saveState mechanism OBS already uses).

**Risk to retire first:** nesting may make interactive resizing glitchy (likely why OBS
ships with it off). The prototype verdict decides everything downstream.

## Roadmap candidates (post-prototype, in rough order)
- Layout presets: save/restore named dock layouts, one click (jrDockie exists but is
  clunky; ours would be branded, polished, and layout-nesting aware)
- Starter layouts gallery: "Chat right, tools left", "Podcast", "Just Chatting" etc.
- A settings toggle (on/off without uninstalling), per Joey's options-get-visible-settings rule
- Guided drag hints (first-run tip explaining edge drops vs center drops = tabs)
- macOS build (template supports it; needs a Mac or CI to compile)
- Distribution: obsproject.com forum resource (free, the funnel) + strmrx.com page;
  GPL means source is public, the moat is polish + first-mover + StrmrX brand
- Possible tie-in: LoadoutX advertises DockX (and vice versa); both are "your OBS, your way"

## Business notes (Joey, 2026-07-29)
Joey's read: nobody has done this = there is a business here. First good tool for a job
becomes THE tool in the OBS plugin world. Monetization TBD (plugin itself likely free as
funnel per family playbook; watermark/premium mechanics do not translate directly to GPL
plugin land, needs its own thinking).
