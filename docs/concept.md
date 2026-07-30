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

**v0.4.0 (2026-07-30):** per-dock colored borders + tinted title bars ("Dock colors" tab),
separator thickness + tint control, hotkey binding INSIDE the dialog (no OBS settings trip),
and Auto switch: scene -> layout rules so the whole dock arrangement changes with the scene
(also the zero-effort Stream Deck integration: switch scene or press the layout hotkey).

**Direction (Joey, 2026-07-29): DockX replaces LoadoutX.** The native plugin does
everything the browser dock did, better, with zero setup (no websocket password dance).
LoadoutX is frozen; its features migrate here.

## Validated demand (researched 2026-07-30: OBS ideas portal votes + plugin downloads)
Ranked by demand x feasibility for a Qt frontend plugin. Context: #1 idea sitewide has
431 votes, so 80-150 = top tier.

1. **Scene folders** -- collapsible folder tree for scenes. 82+23 votes, years of forum
   threads. Incumbent (Scene Tree Folder plugin, ~7.6k downloads) is weak/Windows-only
   with UX complaints. Natural extension of our Scenes panel work. MEDIUM.
2. **Source docks / preview-as-a-dock** -- render any source/scene live inside a dock.
   87 votes; Exeldro's Source Dock has 227k downloads (biggest demand proof found).
   Unlocks multiview/studio-mode/grid follow-ons. MEDIUM.
3. **Cross-collection copy** -- copy scenes/sources between scene collections without
   export files. 149 votes (highest plugin-territory idea). MEDIUM.
4. **Reorderable audio mixer** -- drag to reorder mixer channels. 32+29 votes, constant
   forum pain (people rename sources "1 Mic, 2 Game"). MEDIUM.
5. **Filter hotkeys** -- hotkey to toggle any filter (Stream Deck bait). 59 votes; only a
   Lua script exists today. EASY -- cheapest high-demand win, good v0.5 candidate.
6. **Multiview upgrades** -- pick/order scenes, custom grids, always-on-top. ~130 votes
   combined. MEDIUM-HARD (needs #2's display machinery).
7. **Scene thumbnails** -- previews next to scene names. 22 votes; pairs with folders =
   visual scene browser nobody ships. MEDIUM.
8. **Align/distribute tools** -- snap grid idea has 96 votes; align/distribute buttons are
   EASY (transform math), grid overlay HARD without our own preview dock.
9. **Projector management** -- remember position/borderless/always-on-top (48+26 votes);
   tie projectors into saved layouts = very on-brand. MEDIUM.
10. **Studio mode QoL** -- resizable split (35), hide transition panel (28). MEDIUM.
11. **Scene collection zip export/import** with media (38 votes). MEDIUM, overlaps #3.
12. **Profile + collection linked switching** ("Super-Profile", 32+23 votes). EASY;
    extends our Auto switch naturally.
13. **Source tagging + bulk ops + find-usages** -- low votes but constant forum pain;
    compounds our search bars. EASY-MEDIUM.
14. **Managed dock container on a second monitor** (21 votes). MEDIUM; deepens the moat.

Strategic reads: folders+thumbnails (#1+#7) = most wanted organization feature in OBS
history with a beatable incumbent; #2 is proven at 227k downloads and unlocks 6/8/10;
"planned" status on the OBS portal has meant years of nothing (ship first). Excluded as
off-thesis: encoders, multi-output, NDI, VST, replay buffer.

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
