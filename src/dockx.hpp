/*
DockX for OBS Studio (by StrmrX) -- shared declarations.
GPL v2, see plugin-main.cpp for the full notice.
*/
#pragma once

#include <obs.h>

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QString>

#include <vector>

namespace dockx {

struct Layout {
	int id = 0;
	QString name;
	QByteArray state; /* QMainWindow::saveState blob */
	obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
};

struct State {
	/* settings (all user visible, defaults ON) */
	bool nesting = true;
	bool sceneSearch = true;
	bool sourceSearch = true;
	bool sceneColors = true;

	int nextId = 1;
	std::vector<Layout> layouts;
	QHash<QString, QString> colors; /* scene name -> "#rrggbb" */
	QByteArray undoState;           /* layout snapshot taken before the last apply */
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
bool applyLayout(int id);
bool undoLayout();
void shutdown();
} // namespace panels

void showDialog();

/* colored dot icon for a scene row; theme stylesheets cannot override icons,
   so the color always shows even when the theme repaints item text */
QIcon colorDot(const QColor &c);

} // namespace dockx
