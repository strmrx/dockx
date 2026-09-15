# OBS Forum Resource listing - DockX (draft for Joey to post)

This is the copy for the obsproject.com **Resources** listing. You (Joey) create the
resource from the StrmrX forum account. Claude does not post to moderated platforms.

## Fields to fill in on the "Add resource" form

- **Title:** `DockX - Flexible Dock Layouts for OBS`
- **Tag line:** `Full-height chat beside stacked panels, saved layouts, editable preview dock, and more. Free.`
- **Category:** Plugins (OBS Studio Plugins / Tools)
- **Version:** `0.55.1` (match buildspec.json at launch)
- **Icon:** attach `brand/dockx-icon-256.png`
- **Screenshots (attach 3-5):** you will need these before posting -
  1. A full-height chat column beside a stacked panel column (the headline layout)
  2. The Tools > DockX window on the Layouts tab
  3. Scene Folders dock with thumbnails
  4. The DockX Preview editable dock
  5. Colors / a one-click look applied
- **Download URL / file:** point the resource's download at the GitHub Release asset
  (github.com/strmrx/dockx/releases) once you publish the tagged release. The forum listing
  then becomes the discovery + download-count funnel.

## Description (paste as BBCode)

```bbcode
[B]DockX unlocks the dock layouts OBS never let you build.[/B]

OBS docks live in rows, so you cannot put a full-height chat column next to a second column of
stacked panels. The framework OBS is built on has supported nested docks for years. OBS just
never turned it on. DockX turns it on from inside OBS, then adds the layout tools OBS always
needed. It all lives under [B]Tools > DockX[/B].

DockX is free, with every feature unlocked.

[B]What it does[/B]
[LIST]
[*][B]Flexible dock nesting[/B] - full-height columns beside stacked panels.
[*][B]DockX Preview[/B] - an editable canvas inside a dock: move, resize, rotate and crop sources from a panel, with snapping, studio mode, and per-dock zoom.
[*][B]Scene Folders[/B] - a searchable folder tree over your scenes, with live thumbnails and a grid view.
[*][B]Saved layouts and hotkeys[/B] - snapshot an arrangement and bring it back with a key, or auto-switch it with the scene. Lock a layout so it never drifts mid stream.
[*][B]Source loadouts[/B] - save where every source sits and snap them all back in one click.
[*][B]Find any source[/B] - one search box scans every scene at once.
[*][B]Source tags + bulk actions[/B] - label sources, then Show / Hide / Lock / Mute a whole group across every scene.
[*][B]Wide docks[/B] - stretch a dock across a whole row.
[*][B]Pin any window[/B] - dock an outside app (a TikTok Live Studio chat, a browser, a music player) into your OBS layout.
[*][B]Second-screen container[/B] - gather panels into one managed, nestable window on another monitor.
[*][B]Colors and themes[/B] - recolor OBS to your brand with one-click looks.
[/LIST]

[B]Install (Windows)[/B]
Download the installer, close OBS, and run it. Or grab the zip and run install.bat as
administrator. Then open OBS and look under Tools > DockX. DockX is a Windows plugin for now.

[B]A note on the Windows security prompt[/B]
Windows may show a "Windows protected your PC" box because DockX is new software from a small
publisher without a paid code-signing certificate yet. It does not mean anything is wrong.
DockX is open source, so you can read every line. Click "More info" then "Run anyway".

[B]Free and open source[/B]
DockX is free with every feature unlocked, licensed GPL v2. Source: https://github.com/strmrx/dockx
If it earns a spot in your setup, you can support development on Ko-fi: https://ko-fi.com/strmrx

Guides and download: https://strmrx.com/dockx
Made by StrmrX - tools built by streamers, for streamers: https://strmrx.com
```

## After posting

Set `DOWNLOAD_URL` in `strmrx-hub-repo/src/pages/dockx.astro` to the forum resource URL so
the site's Download button routes through the listing (the download-count funnel), then deploy
the hub (git push origin main).
```
