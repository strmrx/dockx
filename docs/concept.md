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
3. **Cross-collection copy** -- SHIPPED v0.17 (2026-08-01, 149 votes, highest
   plugin-territory idea). Right-click a scene in the folder dock -> "Copy to
   Collection" -> pick a target. Works with NO export files and no collection
   switch, by additively editing the target collection's saved JSON on disk
   (dockx-collections.cpp): OBS only reads a collection file when it loads it, so
   an inactive file is safe to append to. SAFETY: never the active collection;
   ADDITIVE ONLY (a source whose name exists in the target is left alone, the
   scene binds to it; the scene is unique-named if taken); source uuids stripped
   so no cross-file uuid clash; a full .dockx-bak written before any write; save
   is atomic (obs_data_save_json_safe). Recurses groups + nested scenes. Seen
   after switching to the target. LIMITATION: on-disk path via
   os_get_config_path_ptr, so OBS portable mode may not resolve (fails safe with
   "could not find on disk").
4. **Reorderable audio mixer** -- SHIPPED v0.8 (drag rows in the DockX dialog Mixer
   tab; order persists + reapplies on every mixer rebuild; in-mixer dragging maybe
   later). 32+29 votes, constant forum pain (people rename sources "1 Mic, 2 Game").
5. **Filter hotkeys** -- SHIPPED v0.5. Hotkey to toggle any filter (Stream Deck bait).
   59 votes; only a Lua script existed. EASY.
6. **Multiview upgrades** -- pick/order scenes, custom grids, always-on-top. ~130 votes
   combined. MEDIUM-HARD (needs #2's display machinery).
7. **Scene thumbnails** -- previews next to scene names. 22 votes; pairs with folders =
   visual scene browser nobody ships. MEDIUM.
8. **Align/distribute tools** -- SHIPPED v0.18 (2026-08-01, snap grid idea has 96 votes).
   New Align tab (dockx-align.cpp): line up edges (left/center/right, top/middle/bottom),
   space evenly across/down (3+ items), center on canvas (h/v/both) for the SELECTED
   sources in the current scene. Works off each item's on-canvas bounding box
   (obs_sceneitem_get_box_transform) so scale/crop/rotation are respected; only translates
   (never resizes), skips locked items. Settings toggle gates the tab, DEFAULT OFF
   (opt in -- don't force layout design on people; first-run guide points to it).
   STILL OPEN: the snap-grid OVERLAY (HARD without our own preview dock).
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
- **Edge strip span controls** (Joey 2026-09-10: "it would be cool to be able to have a
  tab still span across how i want. at the very least at the full horizontal, and even
  better, allow to choose how many docks across it could span... to the bottom and top
  as an option"). Two tiers:
  (a) SMALL: per-edge corner ownership toggles ("Bottom row spans the full window",
  same for top) via QMainWindow::setCorner -- finer control than OBS's all-or-nothing
  Full-height docks menu item (which is exactly what blocked his full-width mixer
  drop). Needs a reapply guard: OBS's own toggle rewrites corners.
  (b) BIGGER: "stretch this dock under..." on a dock's right-click -- choose how many
  neighbor columns an edge strip spans (his old mixer spanned exactly the two chat
  columns). Buildable with the programmatic re-nesting machinery proven by the v0.46.12
  column repair (addDockWidget/splitDockWidget/tabify + saveState rollback).
- **DockX stats dock** (Joey 2026-09-09: "yes build the dockx stats bar please") --
  SHIPPED v0.43.0. New "DockX Stats" dock (new file `src/dockx-stats.cpp`, registered
  at load, opened from the Docks menu): the same health numbers as OBS's Stats panel
  (FPS vs target, CPU, memory, disk free at the recording path, render time,
  render/encode lag, stream status + bitrate + dropped %, recording status + bitrate)
  from the same public counters, polled once a second, read only. The difference is it
  RESPONDS to its size instead of demanding ~590px: wide = two pairs per line, narrow =
  stacked label/value rows, tiny (under ~205px) = essentials only (FPS, CPU, Stream,
  Recording); minimum width 120px with a scroll fallback. Values color amber/red at
  warning/trouble thresholds (lag or drops over 1%/5%, disk under 10GB/1GB, FPS under
  95%/80% of target). Reset button restarts the counters. "A stats panel that actually
  fits in a corner." v0.44.0 (same day): "Dropped (network)" became its own essential
  row with the full count ("12 / 4000 (0.3%)", "-" while not streaming), and the Video
  docks tab gained a "Stream health" box with an "Open DockX Stats" button so the dock
  is discoverable from Tools > DockX, not just the Docks menu.
- **Resize blocker hint** (Joey 2026-09-09: "cool idea... love it"): when a dock
  divider drag hits a wall, tell the user WHICH dock's minimum size is blocking (e.g.
  a toast or a brief highlight on the stubborn dock). Docks can only shrink to the
  widest minimum in their row/column; today that wall is invisible and reads as a bug.
- Scene Folders visual pass (Joey: must read as large/clear as the native panel) + his
  v0.4/v0.5 test feedback
- Source docks / preview-as-a-dock (demand #2, 227k downloads proof; unlocks multiview,
  align tools, studio mode QoL later)
- Scene thumbnails -- SHIPPED v0.16 (2026-08-01, demand #7). Live scene previews power
  the folder GRID view (grid = a visual scene browser). Any scene renders to a 160x90
  offscreen texture (obs_source_video_render -> gs_texrender -> gs_stagesurface readback,
  all GPU objects created + destroyed in one obs_enter/leave_graphics so nothing leaks at
  exit), cached by uuid in dockx-thumbs.cpp. Tiles fill in progressively (one render per
  120ms timer tick) so a big collection never hitches; colored placeholder until ready.
  Cache invalidates on scene-collection change; "Refresh thumbnails" on the grid bg menu;
  Settings toggle (default on). Tree/list view unchanged. NEXT: thumbnails in tree rows.
- Grid mode for Scene Folders -- SHIPPED v0.10 (file explorer model: colored tiles,
  enter folders, Up tile, flattened search, full context menus; drag in grid = later).
  Pairs with scene thumbnails = the visual scene browser.
- Nested folders -- SHIPPED v0.10 (paths under the hood, drag folder into folder,
  optional via Settings toggle, subtree safe rename/delete).
- Profile + collection linked switching (demand #12, EASY, extends Auto switch)
- Cross-collection copy of scenes/sources (demand #3, 149 votes)
- Reorderable audio mixer (demand #4)
- Source tagging / bulk ops / find-usages (demand #13) -- find-usages SHIPPED v0.20
  (project-wide source search); tagging + bulk ops still open
- Align + distribute tools -- SHIPPED v0.18 (align edges + space evenly + center on
  canvas, Align tab, dockx-align.cpp; snap-grid overlay still open)
- Multi-monitor dock manager -- SHIPPED v0.19 (2026-08-01, demand #14 + Joey ask).
  Monitors tab in Tools > DockX: pick a screen, pick one or more docks, "Send docks
  to monitor" floats + tiles them onto that screen (grid, respects availableGeometry);
  their positions then save with any dock layout. Two safety nets so a dock is never
  lost: (a) every layout apply runs monitors::validateVisible() so a layout saved for
  more screens than are attached can't strand a dock off canvas; (b) DockX listens for
  QGuiApplication::screenRemoved and, 400ms after a monitor is unplugged, auto-rescues
  any floating dock whose title bar sits on no screen back onto the OBS screen (cascaded).
  A manual "Rescue lost docks to this screen" button + Tools menu item does the same on
  demand -- the elegant recovery for someone with no saved layout to fall back on.
  Auto rescue has a Settings toggle (default ON). Handles 2, 3, N monitors. New file
  dockx-monitors.cpp; setting auto_rescue persisted in dockx.json.
- Project-wide source search -- SHIPPED v0.20 (2026-08-03, demand #13 find-usages +
  Joey ask). New Find tab (first tab in Tools > DockX) + a "DockX: Find source" Tools
  menu item that opens straight to it. One search box scans every scene in the collection
  at once (group children + nested scenes descended), matching source name, type, or
  scene. Results are name / type / in-scene (with a scene › group breadcrumb when nested);
  double-click or "Go to source" switches to that scene and selects the item on canvas.
  Also surfaces sources that are loaded but placed in no scene (listed "(unused)") -- OBS
  itself can't tell you this. In-memory filter over one scan (Refresh re-scans). New file
  dockx-search.cpp (search::findAll + search::reveal); no persisted state.
  v0.20.1: Find results are actionable -- Properties (open any source's settings, and
  what double-click does on an orphan since there's no scene to jump to) and Delete
  source (remove it from the collection; confirm shows scene-use count). Makes the
  (unused) list a real cleanup tool for orphaned sources OBS can't otherwise show.
  v0.20.2: right-click context menu (Go to source / Properties / Remove from scene /
  Delete from project); Remove from scene drops one scene item (obs_sceneitem_remove)
  without leaving DockX; delete now verifies (re-lookup after obs_source_remove) and
  tells the user honestly when a source can't be freed because something still holds it.
  v0.20.3: OBS skips removed sources on save (obs.c:2510), so delete is durable even if a
  live ref lingers -- deleted sources now hide from the list at once (per-session removed
  set), and search::describeHolders names what still holds a source (DockX dock / global
  audio assignment / filter parent) with the fix, honest fallback for un-introspectable refs.
- Projector management (#9), multiview upgrades (#6), studio mode QoL (#10),
  collection zip export/import (#11)
- **Collapse main preview + Program/Preview dock as the replacement** (Joey 2026-08-07).
  SHIPPED v0.21.0 (2026-08-27). OBS's main video canvas is the QMainWindow central widget
  (not a dock): it has a hard minimum size and every dock can only ring it, so it always
  claims a fixed block in the middle. v0.21 hides the central widget so docks reclaim the
  whole window: Settings toggle + a "DockX: Collapse or expand preview" Tools item; on
  collapse, if no Program/Preview source dock exists, DockX offers to add a Program dock
  as the replacement view. Enabling studio mode auto-expands the preview (studio edits
  need the canvas). Resource-neutral (a hidden widget stops being exposed so its display
  stops painting; stream/record/sources keep running -- same as OBS's own "Disable
  Preview"). New file dockx-preview.cpp; state preview_collapsed persists across restarts.
  v0.21.1 hotfix: v0.21.0 only hid the widget and OBS kept rendering the preview into the
  hidden window, stalling the graphics pipeline (whole-UI stutter on Joey's rig); collapse
  now also flips obs_frontend_set_preview_enabled, the same switch as Disable Preview.
  Caveat that drives the next item: a Program view is view-only, so a collapsed preview
  loses click-drag scene editing -- the editable Preview dock below is the real payoff.
  2026-09-09, the 0.45.x divider/settling attempt (BUILT, RIG-TESTED, REVERTED same day;
  code in git history at `301ef9f`/`4119b25`, revert `c3a4384`): hiding the central widget
  is why the separator between OPPOSITE dock areas freezes and the layout "settles"/drifts
  (areas only trade space through the center). v0.45.0 pinned the center to zero size
  (visible, min=max 0x0) -- that half WORKED on the rig (settling gone) -- and bridged the
  still-frozen between-areas boundaries with invisible drag handles via
  QMainWindow::resizeDocks, which PROVED to silently refuse cross-area trades. v0.45.1
  swapped the drag to pinning the dock's min=max each mouse move; the rig got display
  abnormalities, a snap-back on release, and a d3d11 graphics-thread crash (no dockx
  frames; suspect: per-move relayouts resizing every video dock's obs_display). Reverted
  to 0.44.0. Next try lives in handoff.md "LESSONS": ship the zero-size-center half alone
  first; any drag mechanism must throttle relayouts and make sizes stick.
- **Fully interactive (editable) Preview dock** (Joey 2026-08-07) -- FIRST SLICE SHIPPED
  v0.22.0, SECOND SLICE (resize + rotate + snap-to-sources) SHIPPED v0.23.0, THIRD SLICE
  (multi-item group resize + Alt-drag edge crop) SHIPPED v0.24.0 (2026-08-28).
  Rebuild OBS's canvas editing inside a DockX dock: the interaction that today lives only
  in OBS's one main preview widget. New file dockx-editpreview.cpp renders the current
  scene through its own obs_display (so it shows video whether or not the main preview is
  enabled -- a passive Program dock goes black off-stream, which is why collapse now
  surfaces/offers THIS instead) and rebuilds the editing on top: click to hit-test the
  topmost source under the cursor and select it (syncs OBS selection), drag to move it
  (single or multi-select), snap the selection bbox to the canvas edges + center AND now
  to every other source's edges/center, with live snap guide lines. v0.23.0 adds full
  single-item transform: 8 resize handles (corners scale both axes, edges scale one; the
  opposite corner/edge stays pinned; scale for normal items, bounds for bounds items) and
  a rotate stalk above the top edge (hold Ctrl to snap to 15 degrees). Handles keep a
  constant on-screen size at any zoom and show only when exactly one unlocked item is
  selected; hover shows the matching resize/rotate cursor. Editable docks persist (state
  edit_docks), add via Tools "DockX: Add DockX Preview (editable)" or Source docks tab,
  manage/remove in that tab. v0.24.0 adds **multi-item group resize** (2+ selected -> handles
  wrap the axis-aligned group bbox; each item scales about the shared anchor, snapping to
  canvas/other sources) and **Alt-drag edge crop** (Alt + an edge handle on a single unrotated
  non-bounds item crops that side in source px, opposite edge pinned; falls back to resize when
  the item can't be cleanly cropped). v0.26.0 fills in the finer editor bits: **group rotate**
  (a rotate stalk on the group bbox spins every selected item about the group center, orbiting
  each item's position + advancing its own rotation; Ctrl snaps 15 degrees), **corner (two-side)
  crop** (Alt + a corner handle crops the two adjacent sides at once, opposite corner pinned), and
  **rotated crop** (crop now runs on the item's own axes, so any-angle sources crop cleanly; the
  old unrotated-only limit is gone). v0.27.0 adds **bounds-fitted crop** (Alt + drag now crops
  bounds-sized sources too: the crop applies with the same frozen start-frame mapping but the box
  is NOT repositioned, so the content refits inside the pinned bounds box -- matching OBS's own
  `CropItem`, which only repositions when OBS_BOUNDS_NONE). v0.28.0 closes the editor: **studio-mode
  staging-scene editing** (in studio mode the DockX Preview edits the PREVIEW/staging scene, so
  edits go live only on transition; OBS's built-in split view is kept -- Joey's call, the DockX
  Preview is NOT the sole studio surface) and **soft angular snapping on rotate** (free rotation
  gently clicks to 15-deg multiples + back-to-original within 5 deg, on both single and group
  rotate; Ctrl still hard-snaps to 15 -- mirrors OBS's own no-modifier RotateItem snapping). The
  DockX Preview editor is now feature-complete; only far-future polish (on-canvas angle readout,
  per-scene "edit this scene" dock, custom guides) remains. The real payoff of the collapse work.
- **Starter layout templates** (Joey 2026-08-28) -- SHIPPED v0.25.0. One-click dock arrangements
  for new users, in `src/dockx-templates.cpp` (`dockx::templates`). NOT a saved blob (every
  user's docks differ) -- it arranges the docks THIS OBS has via the QMainWindow dock API:
  collapses OBS's fixed preview (flexible mode), adds a DockX Preview if none, detects the chat
  dock by name (Twitch/YouTube/Kick browser docks all contain "chat") and OBS's core panels
  (scenes/sources/mixer/transitions/controls), and tiles/tabs them per template. Three ship:
  **Vertical Chat Focus** (tall chat left, preview top-right, panels tiled beneath), **Wide
  Preview** (big preview across the top, chat + panels along the bottom), **Compact** (preview
  left, chat + panels tabbed right). If no chat dock exists, a labeled "add your chat here"
  placeholder holds the slot. Every apply snapshots the layout into state().undoState first, so
  panels::undoLayout() ("Undo apply") reverts it -- safe even fired at a hand-built layout. UI:
  Tools > DockX > Templates tab + a "DockX: Layout templates" Tools item. STILL OPEN: per-user
  saved templates, more starters, and detecting more service dock names as they appear.
- **Placeholder docks + window pinning** (Joey ask 2026-09-04) -- SHIPPED v0.29.0. Joey
  floats the TikTok Live Studio chat over OBS and wanted it to feel docked. A placeholder
  is an empty dock (bg color + centered label, e.g. "TikTok chat") that reserves a spot in
  any layout for a window OBS can't host. On Windows, DockX can PIN that window: it joins
  OBS's own window group (owned window), so it stacks exactly like part of OBS -- just
  above OBS, under whatever app the user selects, hidden when OBS minimizes -- while a
  follower keeps it sized exactly over the placeholder. Placeholder hidden (a layout
  without it) = window tucked away; spot back = window back. Never reparented into OBS
  (that crashes both apps); pin persists across restarts by window title. Right click menu on the dock
  + a Placeholders tab in Tools > DockX. Mac/Linux get the placeholder without pinning for
  now. Generic by design: works for any app floated over OBS, not just TikTok. v0.30.0
  rounds it out: SMART MINIMUM (standard: if the window refuses to shrink below its own
  minimum, the placeholder learns it and the dock can't be dragged smaller -- the layout
  never lies) and SEAMLESS LOOK (opt-in per placeholder: hides the pinned window's title
  bar/border while pinned, restored on unpin), both explained in the tab's hint text.
  v0.31.0 (Joey's first real-use feedback, 2026-09-08) makes re-finding SIZE-AWARE: the pin
  stores the window's size at pick time (`pin_w`/`pin_h`) and re-find rejects any candidate
  much bigger in EITHER dimension -- fixes OBS-starts-first grabbing the app's whole main
  window when panel + main share a title (the old spot-based cap required too-big in BOTH
  dimensions and a tall skinny slot let the main window through). Launch order no longer
  matters; the dock paints "Waiting for: <window>" while the panel isn't out yet. The
  learned smart minimum now auto-forgets every ~15s (relearns in a tick if still real), so
  an app whose innards shrank no longer leaves the dock stuck large; plus a right click
  "Reset size limit" escape hatch. Known hard limits (documented in the tab's hint text):
  a third party app's true minimum size can't be overridden (trim panels inside the app to
  shrink it further), and DockX can't pop a re-docked panel back out (one manual click per
  app restart). A window truly identical to another in title, program AND size stays
  ambiguous by nature -- DockX picks the closest match; a wrong grab is fixed by re-pinning.
  v0.32.0 (2026-09-09) adds **MEDIA IN A SPOT** (Joey ask: branding when showing your OBS):
  a placeholder can show a local image, GIF, or looping muted video instead of a pinned
  window (right click > "Show an image or video here", fit/fill/tile modes, persisted as
  `media_path`/`media_mode`). Images/GIFs paint natively; video reuses the source-dock
  obs_display pattern with a PRIVATE `ffmpeg_source` (never in the user's scenes or mixer,
  always muted). Works on every platform, giving Mac/Linux placeholders a purpose pre-pinning.
  One spot = one occupant: setting media unpins and vice versa. Same version fixes the
  smart-minimum auto-forget: it now PROBES the pinned window (2px nudge) and only forgets a
  minimum the app provably dropped -- the old blind 15s forget made the whole layout visibly
  breathe (shrink/regrow) after applying a layout that squeezed the dock below the window's
  real minimum (Joey's shifting-layout report, 2026-09-09).
- **Visible preview canvas** (Joey 2026-09-09: camera off = "no indicator that a screen
  is there") -- SHIPPED v0.41.0. The DockX Preview now paints its canvas as a real
  screen: pure black backdrop + a dim frame around the canvas edge, drawn before/after
  the scene render, so an empty or all-black scene still shows exactly where the canvas
  sits and how it letterboxes.
- **Video docks tab rebuild** (Joey 2026-09-09: the Source docks tab was "terribly
  confusing as a new user... what is program, what is dockx preview") -- SHIPPED
  v0.40.0. Tab renamed to "Video docks" and rebuilt on the de-wording recipe: list rows
  explain themselves ("Program (what your viewers see)", "(scene)" vs "(source)",
  "DockX Preview (your editable preview)"), the empty list teaches in the empty space,
  and the add area is three explained boxes with the DockX Preview as the purple
  primary star. Its copy now leads with Joey's why: OBS's built in preview is bolted to
  the center of the window and every dock must fit around it; add the DockX Preview,
  hide the big preview (Settings), and the whole layout is yours. "Add Program dock"
  button added (was not reachable from the tab before).
- **Docks tab sections + looks dropdown** (Joey 2026-09-09: background/fade buttons
  "blending into the dock colors"; presets "should be a dropdown menu") -- SHIPPED
  v0.39.0. The Colors > Docks box is fully sectioned (divider + bold heading + dim one
  liner each): Border and title color (with Glow on hover), Background color, Title fade
  (with Shimmer, moved next to the fade buttons so the dependency reads), Lines between
  docks. The built in looks collapsed into ONE "One click looks" dropdown whose items
  carry palette stripe swatches, and grew from 5 to 13 (StrmrX brand purple first, plus
  Vaporwave, Nord, Ocean, Lava, Gold rush, Cherry blossom, Dracula).
- **Saved looks** (Joey 2026-09-09: "the colors area needs some type of save look
  options... save different obs looks") -- SHIPPED v0.38.0. "Save this look" (purple
  primary button in the One click looks box) snapshots the ENTIRE Colors tab under a
  name: per dock colors/backgrounds/title fades, glow + shimmer, dock line thickness +
  tint, and the whole window accent (on/color/everywhere). Each saved look becomes its
  own button next to the presets: click = apply (confirm; scene name colors stay, same
  policy as presets), right click = Rename/Delete, saving an existing name asks to
  replace. Persisted as a `saved_looks` array in dockx.json. Applying re-syncs every
  control on the tab without firing their handlers.
- **Bold accent + everywhere toggle** (Joey 2026-09-09 taste-test: "The changes that are
  happenning are cool, but its very little") -- SHIPPED v0.37.0. The accent went from a
  light touch to the standard look: every button wears a dim accent tint (full accent on
  hover/press/checked), selected tab = full accent, filled slider tracks (sub-page),
  ticked checkboxes/radios, list row hover wash, dock title tint, tool buttons. New
  "Spread to every OBS window" checkbox (`chrome_everywhere`, default OFF, confirm
  dialog warns first): moves the accent block from the main window stylesheet onto the
  APP stylesheet so Settings/Properties/Filters get it too; OBS theme switches replace
  the app stylesheet, so OBS_FRONTEND_EVENT_THEME_CHANGED triggers a delayed reapply
  (`applyChromeSoon`, 200ms). Told Joey why some scroll bars can never change: browser
  docks (chat) draw their own scroll bars inside the web page; the Windows title bar
  belongs to Windows. Same version: the "Lines between docks" controls in Colors > Docks
  got their own divider + bold heading (they blended into the dock color pile).
- **Layouts tab de-wording pass** (Joey 2026-09-09: "this is a scary page... a LOT of
  words, small print, not a great UX") -- SHIPPED v0.36.0, the MODEL for restyling the
  other tabs. The recipe: (1) empty lists explain themselves (new `HintList` widget
  paints centered guidance in the empty space; it vanishes once rows exist); (2) each
  group gets a dim ONE LINE subtitle (`groupSub`) instead of a paragraph of small print;
  (3) one accent styled PRIMARY button per box (`makePrimary`, StrmrX purple) -- Save
  current layout / Save current scene / Pair scene with layout; (4) secondary actions
  (rename, delete, hotkeys, back up, import) folded into a "More" menu button AND a
  right click on the list; (5) long explanations moved into tooltips on the buttons they
  describe; (6) selection dependent buttons stay disabled until a row is picked;
  (7) double click = the main action (apply layout / apply template / restore loadout).
  Visible buttons 15 -> 9 on the tab, hint paragraphs 5 -> 0. Apply the same recipe to
  the remaining tabs in later passes.
- **Whole window accent** (Joey 2026-09-09: "lets do the obs chrome opt in restyle") --
  SHIPPED v0.35.0. New "Whole window accent" group in the Colors tab: an opt-in checkbox
  ("Accent OBS itself (experimental)", `chrome_on`, default OFF) + an accent color picker
  (`chrome_color`, default StrmrX purple). One color layered over OBS's own controls in
  the MAIN WINDOW ONLY: selected tabs, list/tree selections, menus, scroll bar handles,
  slider handles, progress bars, focused fields, hovered buttons, group box titles.
  Implementation = a marker-guarded QSS block appended to the main window stylesheet
  (same trick as the separator tint), so the OBS theme is never replaced and unticking
  strips only our block. Separate windows (Settings, Properties) keep the pure theme by
  design (defensive scope). Applying a Look recolors the accent to match while it is on;
  "Back to theme" turns it off.
- **Glow + shimmer options** (Joey 2026-09-09) -- SHIPPED v0.34.0. Two opt-in checkboxes
  in Colors > Docks: "Glow on hover" (colored docks brighten their border under the
  mouse) and "Shimmer the title fades" (faded title bars slowly swap their two colors,
  ~9s cycle, 120ms retint timer that only runs while enabled). Both default OFF: flair is
  the user's choice.
- **Colors expansion + more tab consolidation** (Joey 2026-09-09) -- SHIPPED v0.33.0.
  Loadouts folded into the Layouts tab (two columns; copy now spells out layouts = your
  PANELS around the screen vs loadouts = your SOURCES inside the scenes -- Joey found the
  split confusing). Scene colors + Dock colors merged into ONE "Colors" tab and expanded
  ("flashy customization gets people talking"): per-dock content BACKGROUND tint
  (`dock_bg_map`), per-dock title FADE into a second color (`dock_grad_map`), and five
  ONE-CLICK LOOKS (Synthwave, Midnight ice, Sunset, Forest, Candy) that cycle a palette
  across every open dock + tint separators, with "Back to theme" to wipe it all. Tab count
  17 -> 12 across the day; Joey wants further consolidation in later rounds.
- **Tools dialog UX pass** (Joey 2026-09-09) -- SHIPPED v0.32.0. (1) The dialog is now
  NON-MODAL: OBS stays fully clickable while it is open, and windows it spawns (Properties)
  come to the front instead of popping up behind it (Joey's Find tab complaint). One window,
  reused if reopened. (2) Templates and Auto switch are no longer separate tabs: both folded
  into the Layouts tab as "Starter templates" and "Auto switch by scene" sections (Joey:
  separate tabs were noise). (3) The Find tab's "Go to source" button becomes "Open
  properties (in no scene)" when an unused source is selected, so the fallback no longer
  reads as "nothing happened".
- **Tools menu slimmed to ONE entry** (Joey 2026-09-04) -- SHIPPED v0.28.1. The Tools menu had
  grown eight "DockX: ..." lines; Joey called it overwhelming/info overload and picked the
  single-entry option. Tools now shows just **"DockX"** (opens the dialog); every removed item
  was already reachable inside it (Find + Templates tabs, Missing media via Settings, Revert /
  Rescue / preview toggle / Add Preview as buttons + hotkeys). Any "DockX: ..." Tools item
  mentioned elsewhere in this doc is pre-0.28.1 history.
- **Starter layout gallery / one-click presets** (Joey 2026-08-07) -- headline onboarding.
  Curated one-click layouts (flagship: "Vertical Chat Focus" -- full-height chat + stacked
  side panels, collapsed preview -> Program dock) so a normal streamer gets the flexible
  layout without hand-wiring docks. Plus "replace this dock with ours" / "replace the main
  preview with ours" as single actions. A preset must create the docks it needs before
  restoreState (which only restores geometry of docks that already exist).

## Build order (Joey, 2026-08-01)
1. Multi-monitor dock manager -- DONE v0.19.
2. **NEXT: "Better OBS Blade"** -- a phone/tablet controller for OBS. RESEARCH DONE
   2026-08-01 (3 verified deep-research passes -> `docs/obs-controller-research.md`).
   Verdict: the standalone feature set is now commoditized (a fresh rival, Control OBS
   v0.1.2 July 2026, already ships hybrid dashboard+grid, panic mute, VU meters, health,
   and remote-over-internet with no port forwarding). The defensible play is NOT a
   standalone "better Blade" -- it's the INTEGRATED version: a cross-platform tablet-first
   HYBRID that is the mobile face of the StrmrX ecosystem, paired with the DockX plugin
   (zero-setup pairing brokered by the account -- kills the #1 pain; plus push health
   alerts to the phone even when the app is closed, which websocket-only rivals can't do).
   Recommended: build AS ControlX mobile (ControlX Ph2 = the relay realized), not a new
   silo. Money: plugin free funnel; paid = server-side (relay, alerts, sync, OBS-anywhere
   backup, metered AI); Aitum-proven ~$5/mo band. A mobile app is NOT GPL-bound.
   **OPEN DECISION (Joey):** ControlX-mobile+DockX vs standalone vs teardown-first vs
   shelve. Also PENDING: dedicated AI-features research pass (got dropped) + a hands-on
   teardown of Control OBS/ProducerPad before committing.
3. **AFTER: cloud sync + "Your OBS anywhere" + AI metering** (the paid layer below),
   plus a deeper monetization research pass on everything else DockX could earn from
   once it pulls real traffic. Server-side value = fork-proof; that is the business.
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
  assets/themes/ for the strmrx.com/dockx page.
- Loadout backup / import -- SHIPPED v0.15 (2026-08-01): "Back up to file" writes all
  loadouts to a shareable JSON (obs_data_save_json_pretty_safe); "Import from file"
  appends them with fresh local ids, never overwriting. This was LoadoutX's last
  unmatched feature (file export/import for portability + sharing). With it, LoadoutX
  is 100% replaced -- Joey retired it 2026-08-01 (folder, hub card, Railway, repo).
- Guided drag hints -- SHIPPED v0.11 (illustrated Dock Layout Guide dialog: title bar
  grab, edge drop = split, center drop = tabs, DockX columns; auto opens once on first
  run, reopenable from Settings, linked to strmrx.com/dockx)
- strmrx.com/dockx help page -- BUILT 2026-08-29 (in the hub repo,
  `strmrx-hub-repo/src/pages/dockx.astro`; Astro page in the house design system). The
  Help "?" button in the Scene Folders dock + the guide dialog already point here. Covers
  what DockX does, the three starter layouts (CSS diagrams), the free StrmrX Midnight theme
  download (served from `public/dockx/StrmrX_Midnight.ovt`), a Getting-started guide, the
  full Uninstall / revert story, and the conversion doorway into the StrmrX family.
  Carries a `SoftwareApplication` schema node and is in the sitemap. ONE thing pending
  launch: the download CTA is a single swappable `DOWNLOAD_URL` constant, empty for now so
  the button shows an honest "in final beta -> /beta" state; set it to the obsproject.com
  resource-listing URL at launch (strategy: forum listing = discovery + download counter,
  GitHub Releases hosts the file, this page routes every download through the listing).
  NOT yet committed/deployed on the hub (awaiting Joey; the hub had another dev's WIP in
  the tree this session).
- macOS build -- CI DONE (2026-09-03): GitHub Actions builds Win + macOS universal + Ubuntu
  on every push; version tags draft a Release with all installers. Unsigned Mac builds until
  the Apple Developer account is bought at launch (see the platforms note below).
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
- **All platforms OUT OF THE GATE (Joey, 2026-09-03):** DockX launches on Mac, PC, and
  Linux together. GitHub Actions CI (restored from obs-plugintemplate, 2026-09-03) builds
  all three on every push; code is plain Qt/C++ with no Windows-only pieces. Mac builds are
  UNSIGNED until launch: buy ONE Apple Developer account (~$99/yr, covers every StrmrX app),
  add the signing secrets, and the same CI signs + notarizes automatically. Joey's Mac
  laptop is the Mac test rig (unsigned installs fine via right-click > Open).
