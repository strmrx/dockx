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

**v0.5.0 (2026-07-30):** the two top validated-demand picks, greenlit by Joey. Filter
hotkeys: every filter on every source gets an on/off hotkey automatically (bindings ride
the scene collection), bindable from a new Filters tab. Scene Folders: a native dock with
a collapsible, searchable, drag and drop folder tree over the scene list, keyed by scene
UUID and per scene collection; click to switch, studio mode aware, scene colors carried in.

**Direction (Joey, 2026-07-29): DockX replaces LoadoutX.** The native plugin does
everything the browser dock did, better, with zero setup (no websocket password dance).
LoadoutX is frozen; its features migrate here.

## Validated demand (researched 2026-07-30: OBS ideas portal votes + plugin downloads)
Ranked by demand x feasibility for a Qt frontend plugin. Context: #1 idea sitewide has
431 votes, so 80-150 = top tier.

1. **Scene folders** -- SHIPPED v0.5. Collapsible folder tree for scenes. 82+23 votes,
   years of forum threads. Incumbent (Scene Tree Folder plugin, ~7.6k downloads) is
   weak/Windows-only with UX complaints. MEDIUM.
2. **Source docks / preview-as-a-dock** -- PHASE 1 SHIPPED v0.7, PHASE 2 SHIPPED v0.9
   (audio docks with mute + cubic volume slider, mic strip docks, browser docks fully
   clickable via interaction passthrough; remaining: per-dock zoom, scene item editing
   through a dock). 87 votes; Exeldro's Source Dock has 227k downloads (biggest demand
   proof found). Unlocks multiview/studio-mode/grid follow-ons.
   CORRECTED competitive read (verified 2026-07-30): Source Dock is ALIVE and healthy
   (updated Mar 2026, OBS 32 support, 79% five star), NOT abandoned. Known gaps: Mac
   load failures, crash on exit reports, groups don't list members, year-old open
   issues, utilitarian UI. Our angle = integration (docks + layouts + hotkeys +
   folders + auto switch in one download) + polish, not rescue of a dead plugin.
3. **Cross-collection copy** -- copy scenes/sources between scene collections without
   export files. 149 votes (highest plugin-territory idea). MEDIUM.
4. **Reorderable audio mixer** -- SHIPPED v0.8 (drag rows in the DockX dialog Mixer
   tab; order persists + reapplies on every mixer rebuild; in-mixer dragging maybe
   later). 32+29 votes, constant forum pain (people rename sources "1 Mic, 2 Game").
5. **Filter hotkeys** -- SHIPPED v0.5. Hotkey to toggle any filter (Stream Deck bait).
   59 votes; only a Lua script existed. EASY.
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

## Roadmap (in rough order; folds in the validated demand list above -- Joey 2026-07-30:
## ALL remaining items from that list stay on this roadmap; DockX is the ONE mega plugin,
## every OBS QoL fix ships in this single download, never ten separate plugins)
- Scene Folders visual pass (Joey: must read as large/clear as the native panel) + his
  v0.4/v0.5 test feedback
- Source docks / preview-as-a-dock (demand #2, 227k downloads proof; unlocks multiview,
  align tools, studio mode QoL later)
- Scene thumbnails in the folder tree (demand #7; folders + thumbnails = the visual
  scene browser nobody ships)
- Grid mode for Scene Folders -- SHIPPED v0.10 (file explorer model: colored tiles,
  enter folders, Up tile, flattened search, full context menus; drag in grid = later).
  Pairs with scene thumbnails = the visual scene browser.
- Nested folders -- SHIPPED v0.10 (paths under the hood, drag folder into folder,
  optional via Settings toggle, subtree safe rename/delete).
- Profile + collection linked switching (demand #12, EASY, extends Auto switch)
- Cross-collection copy of scenes/sources (demand #3, 149 votes)
- Reorderable audio mixer (demand #4)
- Source tagging / bulk ops / find-usages (demand #13)
- Align + distribute tools (#8), projector management (#9), multiview upgrades (#6),
  studio mode QoL (#10), collection zip export/import (#11), second-monitor dock
  container (#14)
- Scene sorting -- SHIPPED v0.11 (drag scenes to reorder in the folder tree, pushed
  into the native list so it sticks; "Sort scenes A to Z" per folder + "Sort all
  scenes A to Z" on the dock background, tree and grid)
- Missing Media cleaner -- SHIPPED v0.13 (Joey ask 2026-07-31: OBS's built in
  Missing Files dialog only relinks or cancels, never lets you just delete a dead
  source). Lists every source with a missing file; Relink / Remove source / Remove
  source + delete file (when the file exists) / Remove all. Tools menu + Settings
  button. Optional startup auto-pop, OFF by default (can't suppress OBS's own popup,
  so it stays a choice). Uses obs_source_get_missing_files + obs_source_remove.
- Layout locking -- SHIPPED v0.14 (Joey ask 2026-08-01: lock in a layout and snap
  back if it drifts). New Locks tab, two independent dock modes: a HARD lock
  (docks can't be dragged/floated -- toggles QDockWidget Movable/Floatable features)
  and a SOFT lock (Set revert point captures the arrangement, Revert to point snaps
  it back while docks stay draggable -- his mid-stream mode). Revert reachable three
  ways: Locks tab button, Tools > DockX: Revert dock layout, and a hotkey. Also lock
  ALL sources in a scene / every scene / a checklist of selected scenes at once
  (wraps the v0.12 loadouts::lockScene/lockAll, finally surfaced with buttons +
  lock/unlock current scene hotkeys). Hard lock + revert point persist in dockx.json;
  hard lock reasserts on startup and after any layout apply/revert.
- Starter layouts gallery: "Chat right, tools left", "Podcast", "Just Chatting" etc.
- Layout export/share (streamers trading layouts = the community moment + marketing)
- LoadoutX parity -- SHIPPED v0.12 (2026-07-30): source loadouts (save/restore all
  transforms + visibility/lock, undo = redo, Loadouts tab), lock tools (per scene +
  everywhere), source rows under scenes in the folder dock (eye/lock icons, double
  click toggle, right click controls, search matches sources = find-usages), and the
  live-guarded Switch tab (profiles + collections). StrmrX Midnight theme staged in
  assets/themes/ for the strmrx.com/dockx page. LoadoutX is now fully replaced;
  retire it after Joey's parity check.
- Guided drag hints -- SHIPPED v0.11 (illustrated Dock Layout Guide dialog: title bar
  grab, edge drop = split, center drop = tabs, DockX columns; auto opens once on first
  run, reopenable from Settings, linked to strmrx.com/dockx)
- strmrx.com/dockx help page (the Help "?" button in the Scene Folders dock and the
  guide dialog already point there; page = guides + layout ideas + theme download +
  the conversion doorway into the StrmrX family). NEEDS BUILDING on the hub.
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

## Business model -- DECIDED (Joey, 2026-07-30)
- **The plugin is free, forever, StrmrX branded.** DockX is the billboard: "by StrmrX"
  in the Tools menu of every install. No local feature is ever gated (GPL makes local
  gates forkable in an afternoon anyway; the StreamFX backlash is the cautionary tale).
  Once installed it always works; only online extras follow a membership.
- **Tip jar alongside** (Ko-fi etc. on the obsproject listing + in the dialog). Tips go
  to Joey personally as the solo dev; framed as tips, never purchases (no promised
  perks = no obligations). Loop Willy in on the framing since StrmrX is co-owned.
- **The paid layer is cloud, on StrmrX accounts (S/X tiers).** GPL covers code, not
  services; nobody can fork a server. Planned ladder:
  1. Layout + scene-folder sync across machines (small data, easy win).
  2. Layout sharing gallery (streamers trading layouts; community + marketing moment).
  3. **"Your OBS anywhere" -- the big one (Joey + Willy had wanted this separately):**
     full OBS setup backup/restore in the cloud. Travel and connect into your own OBS
     setup anywhere; PC dies, hit one button on the new machine and your setup is back.
     Overlaps roadmap #11 (collection zip export/import with media). Media can be GBs,
     so storage tiers map naturally to S/X. Small per-user cloud element storage too.
- **Aitum precedent:** free GPL plugins became defaults for thousands of streamers and
  funnel into their paid product. Same play.
- **macOS (and Linux) later:** the plugin template ships CI that builds all three
  platforms; our code is plain Qt/C++ with no Windows-only pieces. Needs GitHub Actions
  setup + an Apple Developer ID (~$99/yr) for signing/notarization so Mac installs are
  painless. Do after the Windows feature set settles.
