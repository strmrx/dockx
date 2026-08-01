/*
DockX for OBS Studio (by StrmrX) -- shared declarations.
GPL v2, see plugin-main.cpp for the full notice.
*/
#pragma once

#include <obs.h>

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

class QListWidget;
class QWidget;

namespace dockx {

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

	int nextId = 1;
	std::vector<Layout> layouts;
	QHash<QString, QString> colors;       /* scene name -> "#rrggbb" */
	QHash<QString, QString> dockColorMap; /* dock key -> "#rrggbb" */
	QHash<QString, int> sceneLayouts;     /* scene name -> layout id (auto switch) */
	int sepSize = 0;                      /* px between docks; 0 = theme default */
	QString sepColor;                     /* separator tint; empty = theme default */
	QHash<QString, FolderData> folders;   /* scene folders, keyed by collection name */
	QByteArray undoState;                 /* layout snapshot taken before the last apply */
	std::vector<SourceDockEntry> sourceDocks;
	int nextSourceDockId = 1;
	QStringList mixerOrder; /* custom Audio Mixer order (source names, top first) */
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

Layout *findLayout(int id);
Layout &addLayout(const QString &name, const QByteArray &blob);
void removeLayout(int id);
void renameLayout(int id, const QString &name);

namespace panels {
void initAfterLoad();   /* one time UI wiring once OBS finished loading */
void applyNesting();    /* honor state().nesting on the main window */
void applySearchBars(); /* create/show/hide the injected search boxes */
void refreshSoon();     /* debounced: reapply colors + active filters */
void applyDockColors();  /* colored border + title bar per tagged dock */
void applySeparators();  /* thickness/tint of the lines between docks */
void autoSceneLayout();  /* apply the layout mapped to the current scene, if any */
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

void showDialog();

/* the Missing Media cleaner: find sources whose file is gone and get rid of
   them (or relink), the delete button OBS's own missing files dialog lacks */
namespace missing {
int count();                     /* how many sources have a missing file */
void showDialog(QWidget *parent); /* the cleaner window; parent may be null */
void autoPopIfNeeded();          /* startup: pop it if opted in and any missing */
} // namespace missing

/* toggle hotkeys for every filter on every source; bindings are registered on
   the parent source so OBS persists them inside the scene collection */
namespace filters {
struct Entry {
	QString sourceName;
	QString filterName;
	obs_hotkey_id hotkey;
};
void init();               /* signal wiring; call once at module load */
void rescanSoon();         /* debounced reconcile of hotkey registrations */
void applyEnabled();       /* honor state().filterHotkeys */
QList<Entry> entries();    /* current registrations, for the dialog */
void shutdown();
} // namespace filters

/* the Scene Folders dock: collapsible folder tree over the scene list */
namespace folders {
void createDock();   /* register the dock; call once at module load */
void rebuildSoon();  /* debounced tree rebuild from OBS scene list + state */
void applySettings(); /* honor state().folderNewButton */
void showFirstRun(); /* pop the dock open once so people discover it */
void shutdown();
} // namespace folders

/* live video docks: any source/scene (or Preview/Program) rendered in a dock */
namespace sourcedocks {
enum { KIND_SOURCE = 0, KIND_PROGRAM = 1, KIND_PREVIEW = 2 };
void createFromState(); /* register saved docks; call once at module load */
void refreshAll();      /* re-resolve sources after scene/collection changes */
void addDock(int kind, const QString &sourceName);
void removeDock(int id);
void shutdown(); /* MUST run at EXIT, before graphics dies */
} // namespace sourcedocks

/* source loadouts + lock tools (LoadoutX's last features, done natively) */
namespace loadouts {
struct RestoreReport {
	int restored = 0;
	QStringList missing; /* "Scene: Source" rows that no longer exist */
};
SourceLoadout capture(const QString &sceneUuid, const QString &sceneName);
RestoreReport restore(const SourceLoadout &l); /* snapshots an undo first */
bool undoRestore(RestoreReport &report);       /* undo twice = redo */
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
void registerHotkeys();          /* register the frontend hotkeys; idempotent */
void unregisterHotkeys();
void loadHotkeys(obs_data_t *d);  /* restore saved key bindings */
void saveHotkeys(obs_data_t *d);

/* dock layout */
bool hasLockPoint();
void setLockPoint();      /* capture the current dock arrangement */
bool revertToLockPoint(); /* snap docks back to the saved point */
void setHardLock(bool on); /* freeze/unfreeze dock dragging + floating */
bool hardLock();
void applyHardLock();      /* reassert state().hardLock onto the docks */

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
QPixmap cached(const QString &uuid);  /* cache lookup; null if not rendered yet */
QPixmap render(const QString &uuid);  /* render now on the graphics thread + cache */
void invalidateAll();                /* drop the cache (scene collection change) */
void shutdown();                     /* clear the cache; no GPU handles to free */
} // namespace thumbs

/* the dock layout guide: illustrated first-run walkthrough of dock dragging
   (title bar grab, edge drop = split, center drop = tabs, DockX columns) */
namespace hints {
void showFirstRun();            /* opens the guide once after install */
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
