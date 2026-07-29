# DockX -- Concept

## The problem (proven demand)
OBS docks live in rows: side columns hold ONE stack, and everything else sits under the
preview. The layout streamers keep asking for (years-old open request on the OBS ideas
board): a full height chat column PLUS a second column of stacked panels beside it. OBS
cannot do it. No shipped plugin does it. The Qt framework OBS is built on supports it via
"dock nesting"; OBS simply leaves the switch off.

## The product
A native OBS plugin that flips that switch and then productizes layout freedom:

**v0.1 (2026-07-29): VALIDATED.** Dock nesting works on Joey's rig, clean and not janky.
The one existential risk is retired. Full vertical chat + stacked column achieved.

**v0.3 (2026-07-29, shipped same day):** saved dock layouts (apply/rename/delete/undo +
per-layout hotkeys), search bars injected into the native Scenes and Sources panels,
scene name color coding, and a Tools > DockX dialog with visible settings for everything.

**Direction (Joey, 2026-07-29): DockX replaces LoadoutX.** The native plugin does
everything the browser dock did, better, with zero setup (no websocket password dance).
LoadoutX is frozen; its features migrate here.

## Roadmap (in rough order)
- Starter layouts gallery: "Chat right, tools left", "Podcast", "Just Chatting" etc.
- Layout export/share (streamers trading layouts = the community moment + marketing)
- Port LoadoutX features natively: source loadouts (save/restore source positions),
  one click scene lock, profile/collection switching
- Guided drag hints (first-run tip explaining edge drops vs center drops = tabs)
- macOS build (template supports it; needs a Mac or CI to compile)
- Distribution: obsproject.com forum resource (free, the funnel) + strmrx.com page;
  GPL means source is public, the moat is polish + first-mover + StrmrX brand
- The real name: "DockX" is a working title; by feature parity it's bigger than docks
  ("your OBS, your way" is the mission statement, and it was LoadoutX's tagline)

## Business notes (Joey, 2026-07-29)
Joey's read: nobody has done this = there is a business here. First good tool for a job
becomes THE tool in the OBS plugin world. Monetization TBD (plugin itself likely free as
funnel per family playbook; watermark/premium mechanics do not translate directly to GPL
plugin land, needs its own thinking).
