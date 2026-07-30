/*
DockX for OBS Studio (by StrmrX) -- shared declarations.
GPL v2, see plugin-main.cpp for the full notice.
*/
#pragma once

#include <obs.h>

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <vector>

class QListWidget;

namespace dockx {

struct Layout {
	int id = 0;
	QString name;
	QByteArray state; /* QMainWindow::saveState blob */
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
};

/* scene folder layout for one scene collection */
struct FolderData {
	QHash<QString, QString> assign; /* scene uuid -> folder name */
	QStringList order;              /* folder display order */
	QSet<QString> collapsed;        /* folder names currently collapsed */
};

struct State {
	/* settings (all user visible, defaults ON) */
	bool nesting = true;
	bool sceneSearch = true;
	bool sourceSearch = true;
	bool sceneColors = true;
	bool dockColors = true;
	bool filterHotkeys = true;
	bool folderDockIntroduced = false; /* first run pops the Scene Folders dock open */

	int nextId = 1;
	std::vector<Layout> layouts;
	QHash<QString, QString> colors;       /* scene name -> "#rrggbb" */
	QHash<QString, QString> dockColorMap; /* dock key -> "#rrggbb" */
	QHash<QString, int> sceneLayouts;     /* scene name -> layout id (auto switch) */
	int sepSize = 0;                      /* px between docks; 0 = theme default */
	QString sepColor;                     /* separator tint; empty = theme default */
	QHash<QString, FolderData> folders;   /* scene folders, keyed by collection name */
	QByteArray undoState;                 /* layout snapshot taken before the last apply */
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
} // namespace panels

void showDialog();

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
void createDock();  /* register the dock; call once at module load */
void rebuildSoon(); /* debounced tree rebuild from OBS scene list + state */
void showFirstRun(); /* pop the dock open once so people discover it */
void shutdown();
} // namespace folders

/* colored dot icon for a scene row; theme stylesheets cannot override icons,
   so the color always shows even when the theme repaints item text */
QIcon colorDot(const QColor &c);

/* shared preset swatches (dialog palette rows + folder dock context menu) */
extern const char *PRESET_COLORS[8];
extern const char *PRESET_COLOR_NAMES[8];

} // namespace dockx
