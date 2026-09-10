/*
DockX for OBS Studio (by StrmrX) -- shared declarations.
GPL v2, see plugin-main.cpp for the full notice.
*/
#pragma once

#include <obs.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QRect>
#include <QSize>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

class QDockWidget;
class QListWidget;
class QWidget;

namespace dockx {

/* Hand a Qt widget's native window handle to libobs for obs_display creation,
   per platform (mirrors OBS's own QTToGSWindow). Returns false when this
   platform cannot host a display (Linux Wayland needs Qt 6.9+ for a usable
   handle) -- callers must then skip obs_display_create and just log. */
inline bool wireDisplayWindow(gs_init_data &info, quintptr wid)
{
#if defined(_WIN32)
	info.window.hwnd = reinterpret_cast<void *>(wid);
	return true;
#elif defined(__APPLE__)
	info.window.view = (id)wid;
	return true;
#else
	switch (obs_get_nix_platform()) {
	case OBS_NIX_PLATFORM_X11_EGL:
		info.window.id = (uint32_t)wid;
		info.window.display = obs_get_nix_platform_display();
		return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
	case OBS_NIX_PLATFORM_WAYLAND:
		/* Qt 6.9+: winId of a native window IS the wl_surface */
		info.window.display = reinterpret_cast<void *>(wid);
		return info.window.display != nullptr;
#endif
	default:
		return false;
	}
#endif
}

struct Layout {
	int id = 0;
	QString name;
	QByteArray state; /* QMainWindow::saveState blob */
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
};

/* a live video dock: a source/scene (or Preview/Program) rendered in a dock */
struct SourceDockEntry {
	int id = 0;
	int kind = 0; /* 0 = named source/scene, 1 = Program, 2 = Preview */
	QString sourceName;
	double zoom = 1.0; /* 1 = fit the dock; up to 8x punch in */
	double panX = 0.5; /* fraction of the source at the dock's center */
	double panY = 0.5;
};

/* an empty labeled dock that reserves layout space for a window OBS cannot
   own (TikTok Live Studio chat, any app floated over OBS). On Windows a real
   window can be pinned over it and follows the dock */
struct PlaceholderEntry {
	int id = 0;
	QString label;    /* centered text; also the dock title. May be empty */
	QString color;    /* "#rrggbb" background; empty = theme default */
	QString pinTitle; /* title of the window pinned over this dock; empty = none */
	QString pinExe;   /* lower-case exe basename of the pinned app; re-find guard */
	int pinW = 0;     /* the window's size when it was picked: the strongest re-find
	                          tell between an app's panel and its MAIN window when both
	                          share a title (0 = pinned before this was stored) */
	int pinH = 0;
	bool seamless = false; /* strip the pinned window's title bar + border while pinned */
	QString mediaPath;     /* local image/GIF/video shown in the spot; empty = none.
	                           Mutually exclusive with a pinned window */
	QString mediaMode;     /* how the media fills the spot: "fit" (default), "fill", "tile" */
};

/* one captured source state inside a loadout (LoadoutX ported natively) */
struct LoadoutItem {
	QString sceneUuid;
	QString sceneName;
	QString sourceName;
	long long itemId = 0;      /* obs_sceneitem_get_id, stable per collection */
	long long groupItemId = 0; /* the containing group's id; 0 = top level */
	QString groupName;
	double posX = 0, posY = 0, rot = 0;
	double scaleX = 1, scaleY = 1;
	int alignment = 0;
	int boundsType = 0, boundsAlign = 0;
	double boundsX = 0, boundsY = 0;
	bool cropToBounds = false;
	int cropL = 0, cropT = 0, cropR = 0, cropB = 0;
	bool visible = true, locked = false;
};

/* a saved arrangement of sources: position/size/rotation/crop/visibility/lock */
struct SourceLoadout {
	int id = 0;
	QString name;
	QString sceneUuid; /* empty = every scene */
	QString sceneName; /* display only */
	std::vector<LoadoutItem> items;
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID; /* restore hotkey (Joey ask) */
};

/* a user saved color look: a full snapshot of the Colors tab (dock colors,
   backgrounds, title fades, effects, dock lines, whole window accent) that
   applies back in one click, next to the built in looks */
struct SavedLook {
	int id = 0;
	QString name;
	QHash<QString, QString> dockColorMap;
	QHash<QString, QString> dockBgMap;
	QHash<QString, QString> dockGradMap;
	bool dockGlow = false;
	bool gradAnimate = false;
	int sepSize = 0;
	QString sepColor;
	bool chromeOn = false;
	bool chromeEverywhere = false;
	QString chromeColor;
};

/* scene folder layout for one scene collection */
struct FolderData {
	QHash<QString, QString> assign; /* scene uuid -> folder name */
	QStringList order;              /* folder display order */
	QSet<QString> collapsed;        /* folder names currently collapsed */
	QHash<QString, QString> colors; /* folder name -> "#rrggbb" */
};

struct State {
	/* settings (all user visible, defaults ON) */
	bool nesting = true;
	bool sceneSearch = true;
	bool sourceSearch = true;
	bool sceneColors = true;
	bool dockColors = true;
	bool filterHotkeys = true;
	bool folderNewButton = true;       /* the small new folder button in the dock */
	bool folderNesting = true;         /* folders may be dragged into folders */
	bool folderGridMode = false;       /* Scene Folders dock shows tiles, not the tree */
	bool folderDockIntroduced = false; /* first run pops the Scene Folders dock open */
	bool dragHintsShown = false;       /* first run showed the dock layout guide */
	bool folderSources = true;         /* source rows under scenes in the folder dock */
	bool missingAutoPop = false;       /* pop the Missing Media cleaner at startup */
	bool sceneThumbs = true;           /* live scene previews in the folder grid */
	bool alignTools = false;           /* the Align + distribute tab (opt in) */
	bool autoRescue = true;            /* pull stranded docks home on unplug */
	bool previewCollapsed = false;     /* main video preview hidden; docks own the window */
	int edgeTop = 0;                   /* top row corners: 0 = let OBS decide, 1 = row spans
	                                      the full window, 2 = side columns keep the corners */
	int edgeBottom = 0;                /* bottom row corners, same values */

	int nextId = 1;
	std::vector<Layout> layouts;
	QHash<QString, QString> colors;       /* scene name -> "#rrggbb" */
	QHash<QString, QString> dockColorMap; /* dock key -> "#rrggbb" border + title */
	QHash<QString, QString> dockBgMap;    /* dock key -> "#rrggbb" content background tint */
	QHash<QString, QString> dockGradMap;  /* dock key -> second title color (fade) */
	bool dockGlow = false;                /* colored docks brighten their border on hover */
	bool gradAnimate = false;             /* title fades shimmer slowly (opt in, flashy) */
	bool chromeOn = false;                /* accent OBS's own controls (opt in, experimental) */
	bool chromeEverywhere = false;        /* accent reaches every OBS window, not just the main one */
	QString chromeColor = "#8c1eff";      /* the whole window accent color */
	QHash<QString, int> sceneLayouts;     /* scene name -> layout id (auto switch) */
	int sepSize = 0;                      /* px between docks; 0 = theme default */
	QString sepColor;                     /* separator tint; empty = theme default */
	QHash<QString, FolderData> folders;   /* scene folders, keyed by collection name */
	QByteArray undoState;                 /* layout snapshot taken before the last apply */
	std::vector<SourceDockEntry> sourceDocks;
	int nextSourceDockId = 1;
	std::vector<int> editDocks; /* ids of the editable Preview docks */
	int nextEditDockId = 1;
	QHash<int, QString> editDockScenes; /* edit dock id -> pinned scene uuid;
	                                       absent = the dock follows the current scene */
	std::vector<PlaceholderEntry> placeholders;
	int nextPlaceholderId = 1;
	QStringList mixerOrder; /* custom Audio Mixer order (source names, top first) */
	std::vector<SavedLook> savedLooks;
	int nextLookId = 1;
	std::vector<SourceLoadout> loadouts;
	int nextLoadoutId = 1;
	SourceLoadout loadoutUndo; /* pre-restore snapshot; undo twice = redo */
	bool hasLoadoutUndo = false;

	/* lock tools */
	QByteArray lockPoint;  /* dock arrangement to snap back to (soft lock) */
	bool hardLock = false; /* docks frozen: can't be dragged or floated */
	obs_hotkey_id hkRevert = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkLockScene = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkUnlockScene = OBS_INVALID_HOTKEY_ID;
	obs_hotkey_id hkHardLock = OBS_INVALID_HOTKEY_ID;
};

State &state();
void stateLoad();
void stateSave();

/* true only while OBS is fully loaded and NOT switching scene collections.
   Every DockX obs_display draw callback must early-out while this is false:
   rendering a user scene while OBS is still creating (or tearing down) its
   sources races the graphics thread against the loader -- the 2026-09-09/10
   startup crashes (d3d11 access violation in an async source's frame upload,
   4 for 4 within a minute of launch, none after loading finished). */
bool obsReady();
void setObsReady(bool ready);

Layout *findLayout(int id);
Layout &addLayout(const QString &name, const QByteArray &blob);
void removeLayout(int id);
void renameLayout(int id, const QString &name);

namespace panels {
void initAfterLoad();   /* one time UI wiring once OBS finished loading */
void applyNesting();    /* honor state().nesting on the main window */
void applySearchBars(); /* create/show/hide the injected search boxes */
void refreshSoon();     /* debounced: reapply colors + active filters */
void applyDockColors(); /* colored border + title bar per tagged dock */
void applySeparators(); /* thickness/tint of the lines between docks */
void applyChrome();     /* opt in accent color over OBS's own controls */
void applyChromeSoon(); /* delayed reapply, for right after OBS swaps its theme */
void autoSceneLayout(); /* apply the layout mapped to the current scene, if any */
bool applyLayout(int id);
bool undoLayout();
void shutdown();

/* docks currently in the main window, for the dialog list */
struct DockInfo {
	QString key;   /* stable id used in state().dockColorMap */
	QString title; /* what the user sees */
};
QList<DockInfo> listDocks();
QListWidget *nativeSceneList(); /* the native Scenes panel list, or null */
void applyMixerOrder();         /* reorder the native Audio Mixer per state */
QStringList mixerSourceNames(); /* mixer rows in current visual order */
} // namespace panels

void showDialog(const QString &initialTab = QString());

/* the Missing Media cleaner: find sources whose file is gone and get rid of
   them (or relink), the delete button OBS's own missing files dialog lacks */
namespace missing {
int count();                      /* how many sources have a missing file */
void showDialog(QWidget *parent); /* the cleaner window; parent may be null */
void autoPopIfNeeded();           /* startup: pop it if opted in and any missing */
} // namespace missing

/* toggle hotkeys for every filter on every source; bindings are registered on
   the parent source so OBS persists them inside the scene collection */
namespace filters {
struct Entry {
	QString sourceName;
	QString filterName;
	obs_hotkey_id hotkey;
};
void init();            /* signal wiring; call once at module load */
void rescanSoon();      /* debounced reconcile of hotkey registrations */
void applyEnabled();    /* honor state().filterHotkeys */
QList<Entry> entries(); /* current registrations, for the dialog */
void shutdown();
} // namespace filters

/* the Scene Folders dock: collapsible folder tree over the scene list */
namespace folders {
void createDock();    /* register the dock; call once at module load */
void rebuildSoon();   /* debounced tree rebuild from OBS scene list + state */
void applySettings(); /* honor state().folderNewButton */
void showFirstRun();  /* pop the dock open once so people discover it */
void shutdown();
} // namespace folders

/* the DockX Stats dock: OBS's health numbers (fps, cpu, lag, dropped frames,
   bitrates) in a dock that RESPONDS to its size instead of demanding ~590px
   like OBS's own Stats panel. Read only 1s polling; registered at load,
   opened from the Docks menu */
namespace stats {
void createDock();
void showDock(); /* open + raise it (the dialog's discovery button) */
void shutdown(); /* stops the poll timer at EXIT */
} // namespace stats

/* live video docks: any source/scene (or Preview/Program) rendered in a dock */
namespace sourcedocks {
enum { KIND_SOURCE = 0, KIND_PROGRAM = 1, KIND_PREVIEW = 2 };
void createFromState(); /* register saved docks; call once at module load */
void refreshAll();      /* re-resolve sources after scene/collection changes */
void addDock(int kind, const QString &sourceName);
bool showVideoDocks(); /* reopen closed Program/Preview docks; false = none exist */
void removeDock(int id);
void shutdown(); /* MUST run at EXIT, before graphics dies */
} // namespace sourcedocks

/* the editable Preview dock: renders the current scene in a movable dock and
   rebuilds OBS's on-canvas editing (click to select, drag to move, snap) so a
   scene can be edited even while the main preview is collapsed. Renders the
   scene straight (its own display), so it shows video whether or not OBS's main
   preview is enabled -- unlike a passive Program dock, which goes black
   off-stream */
namespace editpreview {
void createFromState(); /* register saved editable docks; call once at load */
void refreshAll();      /* re-point at the current scene after scene changes */
void addDock();         /* add + open a new editable Preview dock */
void removeDock(int id);
bool showDocks();     /* reopen closed editable docks; false = none exist */
QList<int> dockIds(); /* editable dock ids, for the management list */
void shutdown();      /* MUST run at EXIT, before graphics dies */
} // namespace editpreview

/* placeholder docks: an empty colored dock with a label that reserves a spot
   in the layout for an external window. On Windows that window can be PINNED:
   DockX keeps it always on top and moves + sizes it to sit exactly over the
   placeholder, following drags, layout switches and restarts. The foreign
   window is only ever repositioned (SetWindowPos), never reparented, so a
   misbehaving target can never take OBS down */
namespace placeholders {
void createFromState(); /* register saved placeholders; call once at module load */
void addDock(const QString &label);
void removeDock(int id);
void setLabel(int id, const QString &label);
void setColor(int id, const QString &color); /* "#rrggbb" or empty = theme default */
bool pinningSupported();                     /* true on Windows */
void pinWindow(int id, QWidget *parent);     /* pick a running window to pin */
void unpinWindow(int id);
void setSeamless(int id, bool on);         /* hide/restore the pinned window's own frame */
void chooseMedia(int id, QWidget *parent); /* pick a local image/GIF/video to show in the spot */
void clearMedia(int id);
void setMediaMode(int id, const QString &mode); /* "fit" | "fill" | "tile" */
void shutdown();
} // namespace placeholders

/* source loadouts + lock tools (LoadoutX's last features, done natively) */
namespace loadouts {
struct RestoreReport {
	int restored = 0;
	QStringList missing; /* "Scene: Source" rows that no longer exist */
};
SourceLoadout capture(const QString &sceneUuid, const QString &sceneName);
/* register (or re-register after a rename) the loadout's restore hotkey; it
   shows in OBS Settings > Hotkeys as: DockX: restore loadout "name" */
void registerHotkey(SourceLoadout &l);
RestoreReport restore(const SourceLoadout &l);         /* snapshots an undo first */
bool undoRestore(RestoreReport &report);               /* undo twice = redo */
void lockScene(const QString &sceneUuid, bool locked); /* incl. group children */
void lockAll(bool locked);
obs_data_t *toData(const SourceLoadout &l); /* caller releases */
SourceLoadout fromData(obs_data_t *d);
bool exportFile(const QString &path); /* write all loadouts to a JSON backup */
int importFile(const QString &path);  /* append loadouts from JSON; -1 = bad file */
} // namespace loadouts

/* lock tools: freeze the docks so nothing drifts, keep a one-tap revert point
   to snap a moved layout back, and lock every source in a scene (or all
   scenes) at once so nothing on the canvas can be nudged */
namespace locks {
void registerHotkeys(); /* register the frontend hotkeys; idempotent */
void unregisterHotkeys();
void loadHotkeys(obs_data_t *d); /* restore saved key bindings */
void saveHotkeys(obs_data_t *d);

/* dock layout */
bool hasLockPoint();
void setLockPoint();       /* capture the current dock arrangement */
bool revertToLockPoint();  /* snap docks back to the saved point */
void setHardLock(bool on); /* freeze/unfreeze dock dragging + floating */
bool hardLock();
void applyHardLock(); /* reassert state().hardLock onto the docks */

/* scene source locks (wrap loadouts::lockScene/lockAll) */
void lockCurrentScene(bool locked);
void lockAllScenes(bool locked);
void lockScenes(const QStringList &uuids, bool locked);
} // namespace locks

/* cross-collection copy: copy a scene + its sources into another scene
   collection by additively editing that collection's saved JSON on disk
   (never the active one; a .dockx-bak is written first) */
namespace collections {
struct CopyReport {
	bool ok = false;
	QString error;
	int sourcesCopied = 0;
	int sourcesSkipped = 0; /* name already existed in the target */
	QString finalSceneName; /* may be unique-ified if the name was taken */
	QString backupPath;
};
QStringList otherCollections(); /* every collection except the current one */
CopyReport copySceneToCollection(const QString &sceneUuid, const QString &target);
} // namespace collections

/* live scene thumbnails: render any scene (loaded or not, current or not) to a
   small cached preview for the folder grid. All GPU work is per-call (create +
   destroy inside one graphics lock), so nothing persists to clean up at exit */
namespace thumbs {
QSize size();                        /* the thumbnail pixel size */
QPixmap cached(const QString &uuid); /* cache lookup; null if not rendered yet */
QPixmap render(const QString &uuid); /* render now on the graphics thread + cache */
void invalidateAll();                /* drop the cache (scene collection change) */
void shutdown();                     /* clear the cache; no GPU handles to free */
} // namespace thumbs

/* align + distribute the selected sources in the current scene, working off
   each item's on canvas bounding box (scale/crop/rotation respected). Only
   moves items, never resizes; locked items are skipped */
namespace align {
enum Op {
	ALIGN_LEFT,
	ALIGN_HCENTER,
	ALIGN_RIGHT,
	ALIGN_TOP,
	ALIGN_VCENTER,
	ALIGN_BOTTOM,
	DIST_H,
	DIST_V,
};
int selectedCount();                         /* selected, unlocked top-level items in the current scene */
void run(Op op);                             /* align needs >= 2 items; distribute needs >= 3 */
void center(bool horizontal, bool vertical); /* center the selection on the canvas */
} // namespace align

/* multi monitor dock manager: send docks to any screen (tiled), keep saved
   layouts safe across a changed monitor setup, and bring stranded docks home
   the moment a monitor is unplugged so one can never be lost off screen */
namespace monitors {
struct ScreenInfo {
	int index;      /* 0 based screen index */
	QString label;  /* "Monitor 2 · 1920×1080 (main)" */
	QRect geometry; /* full virtual desktop rect */
};
QList<ScreenInfo> listScreens();
int sendDocksToScreen(const QStringList &dockKeys, int screenIndex); /* returns moved */
int rescueStrayDocks(); /* reflow every off screen dock home; returns moved */
void validateVisible(); /* call after restoreState so no dock lands off screen */
void installWatch();    /* listen for monitor unplug; call once after load */
} // namespace monitors

/* project-wide source search: one index of every source across every scene in
   the collection (group children + nested scenes included), so a single box can
   find any source and jump to it. Also surfaces sources loaded but in no scene */
namespace search {
struct Hit {
	QString sourceName;
	QString sourceType; /* friendly type: "Browser", "Image", "Scene", "Group" */
	bool isGroup = false;
	QString sceneUuid; /* empty = source is in no scene (unused) */
	QString sceneName;
	QString groupName;    /* containing group, if nested; else empty */
	long long itemId = 0; /* obs_sceneitem_get_id, for reveal */
	bool visible = true;
	bool locked = false;
};
QList<Hit> findAll();                                    /* every source occurrence across all scenes + unused inputs */
bool reveal(const QString &sceneUuid, long long itemId); /* switch scene + select */
void openProperties(const QString &sourceName);          /* open its OBS Properties */
bool removeFromScene(const QString &sceneUuid, long long itemId); /* drop just this scene item */
bool deleteSource(const QString &sourceName);                     /* remove from whole project; true if fully gone */
QString describeHolders(const QString &sourceName);               /* best-effort: what still holds it live */
} // namespace search

/* collapse the main video preview: OBS's canvas is the QMainWindow central
   widget (docks can only ring it); hiding it hands the whole window to the
   docks, with a live Program/Preview source dock as the movable stand in.
   Stream/record/sources keep running, same as OBS's own "Disable Preview" */
namespace preview {
bool collapsed();
void apply();                           /* reassert state().previewCollapsed on the window */
void setCollapsed(bool on);             /* set + apply + save */
void offerVideoDock(QWidget *parent);   /* offer a Program dock if none exists */
void toggleWithPrompt(QWidget *parent); /* the Tools menu toggle */
void onStudioModeEnabled();             /* expand: studio mode edits need the canvas */
} // namespace preview

/* drag handles for the frozen boundaries between opposite dock areas: while
   the preview is collapsed the central widget is pinned to zero size, so
   Qt's own separator between (say) a left column and a right column cannot
   trade space (areas only negotiate with the center). Thin invisible handle
   widgets overlay exactly those boundaries and do the drag themselves via
   QMainWindow::resizeDocks, both directions. Same-area separators stay
   native and untouched */
namespace divider {
void setActive(bool on); /* driven by preview::apply(); on = build handles */
void shutdown();
} // namespace divider

/* edge row span controls: per-edge corner ownership (does the top/bottom
   row run the full window width, or do the side columns keep the corners)
   with a guard that reasserts the choice when OBS's own Full-height docks
   toggle rewrites the corners; plus "stretch this dock across a row"
   (rebuild the nesting so one dock spans a chosen row of neighbor docks,
   above or below them), offered on every dock title bar right-click and
   from the dialog. Snapshot + verify + rollback, like the column repair */
/* the resize blocker hint: docks only shrink to the largest minimum in
   their row/column, and when a separator drag hits that wall OBS silently
   stops -- the wall is invisible and reads as a bug. A pure observer
   watches native separator drags and, when one is clearly refused, gives
   the dock at its minimum a brief amber flash + a small bubble naming it */
namespace blocker {
void start();                         /* install the watcher; call once after load */
void flashBlocked(QDockWidget *dock); /* flash + bubble on one dock (divider reuses it) */
void shutdown();
} // namespace blocker

namespace edges {
void start();               /* corners + title bar menus; call once after load */
void applyCorners();        /* reassert state().edgeTop/edgeBottom on the window */
void releaseEdge(bool top); /* untick: hand the edge's corners back to OBS's own choice */
void showStretchDialog(QDockWidget *dock, QWidget *parent); /* null dock = pick in the dialog */
void shutdown();
} // namespace edges

/* starter layout templates: one-click dock arrangements for new users (tall
   chat + editable preview + panels, etc). Each detects the docks it can (chat by
   name, OBS's core panels, the DockX Preview), places what it finds, and leaves a
   labeled hint where a dock is missing. Every apply snapshots the current layout
   first (state().undoState), so it reverts with panels::undoLayout() -- safe even
   for a power user who fires one at a hand-built layout */
namespace templates {
struct Info {
	QString id;
	QString name;
	QString desc;
};
QList<Info> list();                             /* the built-in starter templates */
bool apply(const QString &id, QWidget *parent); /* build it; snapshots undo first */
} // namespace templates

/* the dock layout guide: illustrated first-run walkthrough of dock dragging
   (title bar grab, edge drop = split, center drop = tabs, DockX columns) */
namespace hints {
void showFirstRun();             /* opens the guide once after install */
void showGuide(QWidget *parent); /* opens it on demand (Settings tab) */
} // namespace hints

/* where the Help button and guide link send people */
extern const char *HELP_URL;

/* colored dot icon for a scene row; theme stylesheets cannot override icons,
   so the color always shows even when the theme repaints item text */
QIcon colorDot(const QColor &c);

/* shared preset swatches (dialog palette rows + folder dock context menu) */
extern const char *PRESET_COLORS[8];
extern const char *PRESET_COLOR_NAMES[8];

} // namespace dockx
