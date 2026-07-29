/*
DockX for OBS Studio (by StrmrX) -- state, persistence, and panel work.
GPL v2, see plugin-main.cpp for the full notice.

Everything that touches OBS's own widgets lives here and follows one rule:
if the UI does not look the way we expect, do NOTHING. Never crash OBS.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QAbstractItemModel>
#include <QBoxLayout>
#include <QBrush>
#include <QColor>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

namespace dockx {

/* ================= state & persistence ================= */

static State g_state;
State &state()
{
	return g_state;
}

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

static void hotkeyCb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	int id = (int)(intptr_t)data;
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	/* hotkeys can fire off the UI thread; layout changes must not */
	QMetaObject::invokeMethod(
		m, [id]() { panels::applyLayout(id); }, Qt::QueuedConnection);
}

static void registerHotkey(Layout &l)
{
	QByteArray name = QString("dockx_layout_%1").arg(l.id).toUtf8();
	QByteArray desc = QString("DockX: apply layout \"%1\"").arg(l.name).toUtf8();
	l.hotkey = obs_hotkey_register_frontend(name.constData(), desc.constData(), hotkeyCb,
						(void *)(intptr_t)l.id);
}

static char *configFilePath()
{
	return obs_module_config_path("dockx.json");
}

void stateLoad()
{
	char *file = configFilePath();
	obs_data_t *d = obs_data_create_from_json_file(file);
	bfree(file);
	if (!d) {
		obs_log(LOG_INFO, "no saved config, starting fresh");
		return;
	}

	obs_data_set_default_bool(d, "nesting", true);
	obs_data_set_default_bool(d, "scene_search", true);
	obs_data_set_default_bool(d, "source_search", true);
	obs_data_set_default_bool(d, "scene_colors", true);
	obs_data_set_default_int(d, "next_id", 1);

	g_state.nesting = obs_data_get_bool(d, "nesting");
	g_state.sceneSearch = obs_data_get_bool(d, "scene_search");
	g_state.sourceSearch = obs_data_get_bool(d, "source_search");
	g_state.sceneColors = obs_data_get_bool(d, "scene_colors");
	g_state.nextId = (int)obs_data_get_int(d, "next_id");

	obs_data_t *colors = obs_data_get_obj(d, "colors");
	if (colors) {
		for (obs_data_item_t *item = obs_data_first(colors); item;
		     obs_data_item_next(&item)) {
			const char *scene = obs_data_item_get_name(item);
			const char *hex = obs_data_item_get_string(item);
			if (scene && hex && *hex)
				g_state.colors[QString::fromUtf8(scene)] = QString::fromUtf8(hex);
		}
		obs_data_release(colors);
	}

	obs_data_array_t *arr = obs_data_get_array(d, "layouts");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(arr, i);
			Layout l;
			l.id = (int)obs_data_get_int(o, "id");
			l.name = QString::fromUtf8(obs_data_get_string(o, "name"));
			l.state = QByteArray::fromBase64(obs_data_get_string(o, "state"));
			if (l.id > 0 && !l.name.isEmpty() && !l.state.isEmpty()) {
				g_state.layouts.push_back(l);
				Layout &stored = g_state.layouts.back();
				registerHotkey(stored);
				obs_data_array_t *hk = obs_data_get_array(o, "hotkey");
				if (hk) {
					if (stored.hotkey != OBS_INVALID_HOTKEY_ID)
						obs_hotkey_load(stored.hotkey, hk);
					obs_data_array_release(hk);
				}
			}
			obs_data_release(o);
		}
		obs_data_array_release(arr);
	}
	obs_data_release(d);
	obs_log(LOG_INFO, "config loaded: %d layouts, %d scene colors", (int)g_state.layouts.size(),
		(int)g_state.colors.size());
}

void stateSave()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_bool(d, "nesting", g_state.nesting);
	obs_data_set_bool(d, "scene_search", g_state.sceneSearch);
	obs_data_set_bool(d, "source_search", g_state.sourceSearch);
	obs_data_set_bool(d, "scene_colors", g_state.sceneColors);
	obs_data_set_int(d, "next_id", g_state.nextId);

	obs_data_t *colors = obs_data_create();
	for (auto it = g_state.colors.constBegin(); it != g_state.colors.constEnd(); ++it)
		obs_data_set_string(colors, it.key().toUtf8().constData(),
				    it.value().toUtf8().constData());
	obs_data_set_obj(d, "colors", colors);
	obs_data_release(colors);

	obs_data_array_t *arr = obs_data_array_create();
	for (Layout &l : g_state.layouts) {
		obs_data_t *o = obs_data_create();
		obs_data_set_int(o, "id", l.id);
		obs_data_set_string(o, "name", l.name.toUtf8().constData());
		obs_data_set_string(o, "state", l.state.toBase64().constData());
		if (l.hotkey != OBS_INVALID_HOTKEY_ID) {
			obs_data_array_t *hk = obs_hotkey_save(l.hotkey);
			if (hk) {
				obs_data_set_array(o, "hotkey", hk);
				obs_data_array_release(hk);
			}
		}
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "layouts", arr);
	obs_data_array_release(arr);

	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *file = configFilePath();
	if (!obs_data_save_json_safe(d, file, "tmp", "bak"))
		obs_log(LOG_WARNING, "could not save config to %s", file);
	bfree(file);
	obs_data_release(d);
}

Layout *findLayout(int id)
{
	for (Layout &l : g_state.layouts)
		if (l.id == id)
			return &l;
	return nullptr;
}

Layout &addLayout(const QString &name, const QByteArray &blob)
{
	Layout l;
	l.id = g_state.nextId++;
	l.name = name;
	l.state = blob;
	g_state.layouts.push_back(l);
	Layout &stored = g_state.layouts.back();
	registerHotkey(stored);
	stateSave();
	return stored;
}

void removeLayout(int id)
{
	for (auto it = g_state.layouts.begin(); it != g_state.layouts.end(); ++it) {
		if (it->id == id) {
			if (it->hotkey != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(it->hotkey);
			g_state.layouts.erase(it);
			stateSave();
			return;
		}
	}
}

void renameLayout(int id, const QString &name)
{
	Layout *l = findLayout(id);
	if (!l || name.isEmpty())
		return;
	l->name = name;
	/* re-register so the hotkey description in OBS settings shows the new name,
	   carrying any bound keys over */
	if (l->hotkey != OBS_INVALID_HOTKEY_ID) {
		obs_data_array_t *hk = obs_hotkey_save(l->hotkey);
		obs_hotkey_unregister(l->hotkey);
		registerHotkey(*l);
		if (hk) {
			if (l->hotkey != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_load(l->hotkey, hk);
			obs_data_array_release(hk);
		}
	}
	stateSave();
}

/* ================= panels ================= */

namespace panels {

static QPointer<QLineEdit> sceneSearchEdit;
static QPointer<QLineEdit> sourceSearchEdit;
static QPointer<QAbstractItemModel> watchedScenesModel;
static QPointer<QAbstractItemModel> watchedSourcesModel;
static QPointer<QTimer> refreshTimer;
static bool applying = false;
static bool shuttingDown = false;

static QListWidget *sceneList()
{
	QMainWindow *m = mainWindow();
	return m ? m->findChild<QListWidget *>("scenes") : nullptr;
}

static QListView *sourceView()
{
	QMainWindow *m = mainWindow();
	return m ? m->findChild<QListView *>("sources") : nullptr;
}

void applyNesting()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	m->setDockNestingEnabled(state().nesting);
	obs_log(LOG_INFO, "flexible dock layouts %s", state().nesting ? "on" : "off");
}

static void filterScenes()
{
	QListWidget *list = sceneList();
	if (!list)
		return;
	const QString q = sceneSearchEdit ? sceneSearchEdit->text().trimmed() : QString();
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *it = list->item(i);
		it->setHidden(!q.isEmpty() && !it->text().contains(q, Qt::CaseInsensitive));
	}
}

/* each source row is a widget; the first label with text is the source name */
static QString sourceRowName(QListView *view, const QModelIndex &idx)
{
	QWidget *w = view->indexWidget(idx);
	if (!w)
		return QString();
	const QList<QLabel *> labels = w->findChildren<QLabel *>();
	for (QLabel *lbl : labels) {
		const QString t = lbl->text();
		if (!t.isEmpty())
			return t;
	}
	return QString();
}

static void filterSources()
{
	QListView *view = sourceView();
	if (!view || !view->model())
		return;
	const QString q = sourceSearchEdit ? sourceSearchEdit->text().trimmed() : QString();
	QAbstractItemModel *model = view->model();
	for (int r = 0; r < model->rowCount(); r++) {
		bool hide = false;
		if (!q.isEmpty()) {
			const QString name = sourceRowName(view, model->index(r, 0));
			/* rows we cannot read stay visible, never hidden by mistake */
			hide = !name.isEmpty() && !name.contains(q, Qt::CaseInsensitive);
		}
		view->setRowHidden(r, hide);
	}
}

static void applySceneColorsNow()
{
	QListWidget *list = sceneList();
	if (!list)
		return;
	applying = true;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *it = list->item(i);
		const QString hex = state().sceneColors ? state().colors.value(it->text())
						       : QString();
		if (!hex.isEmpty())
			it->setForeground(QBrush(QColor(hex)));
		else
			it->setData(Qt::ForegroundRole, QVariant());
	}
	applying = false;
}

/* OBS rebuilds both lists behind our back (scene switches, renames, collection
   changes). Watch the current models and re-decorate shortly after they move. */
static void watchModels()
{
	QListWidget *scenes = sceneList();
	if (scenes && scenes->model() && scenes->model() != watchedScenesModel) {
		watchedScenesModel = scenes->model();
		QObject::connect(watchedScenesModel, &QAbstractItemModel::rowsInserted, scenes,
				 []() { refreshSoon(); });
		QObject::connect(watchedScenesModel, &QAbstractItemModel::modelReset, scenes,
				 []() { refreshSoon(); });
		QObject::connect(watchedScenesModel, &QAbstractItemModel::dataChanged, scenes,
				 []() {
					 if (!applying)
						 refreshSoon();
				 });
	}
	QListView *sources = sourceView();
	if (sources && sources->model() && sources->model() != watchedSourcesModel) {
		watchedSourcesModel = sources->model();
		QObject::connect(watchedSourcesModel, &QAbstractItemModel::rowsInserted, sources,
				 []() { refreshSoon(); });
		QObject::connect(watchedSourcesModel, &QAbstractItemModel::modelReset, sources,
				 []() { refreshSoon(); });
	}
}

static void refreshNow()
{
	if (shuttingDown)
		return;
	watchModels();
	applySceneColorsNow();
	filterScenes();
	filterSources();
}

void refreshSoon()
{
	if (shuttingDown)
		return;
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	if (!refreshTimer) {
		refreshTimer = new QTimer(m);
		refreshTimer->setSingleShot(true);
		refreshTimer->setInterval(120);
		QObject::connect(refreshTimer, &QTimer::timeout, []() { refreshNow(); });
	}
	refreshTimer->start();
}

/* drop a search box into the panel right above its list */
static QLineEdit *injectSearch(QWidget *listWidget, const char *placeholder)
{
	QWidget *parent = listWidget->parentWidget();
	if (!parent || !parent->layout())
		return nullptr;
	QBoxLayout *box = qobject_cast<QBoxLayout *>(parent->layout());
	if (!box)
		return nullptr;
	int idx = box->indexOf(listWidget);
	if (idx < 0)
		idx = 0;
	QLineEdit *edit = new QLineEdit(parent);
	edit->setPlaceholderText(QString::fromUtf8(placeholder));
	edit->setClearButtonEnabled(true);
	box->insertWidget(idx, edit);
	return edit;
}

void applySearchBars()
{
	QListWidget *scenes = sceneList();
	if (scenes) {
		if (!sceneSearchEdit && state().sceneSearch) {
			sceneSearchEdit = injectSearch(scenes, "Search scenes");
			if (sceneSearchEdit)
				QObject::connect(sceneSearchEdit, &QLineEdit::textChanged, scenes,
						 [](const QString &) { filterScenes(); });
		}
		if (sceneSearchEdit) {
			sceneSearchEdit->setVisible(state().sceneSearch);
			if (!state().sceneSearch && !sceneSearchEdit->text().isEmpty())
				sceneSearchEdit->clear(); /* clearing also unhides all rows */
		}
	}

	QListView *sources = sourceView();
	if (sources) {
		if (!sourceSearchEdit && state().sourceSearch) {
			sourceSearchEdit = injectSearch(sources, "Search sources");
			if (sourceSearchEdit)
				QObject::connect(sourceSearchEdit, &QLineEdit::textChanged, sources,
						 [](const QString &) { filterSources(); });
		}
		if (sourceSearchEdit) {
			sourceSearchEdit->setVisible(state().sourceSearch);
			if (!state().sourceSearch && !sourceSearchEdit->text().isEmpty())
				sourceSearchEdit->clear();
		}
	}
}

bool applyLayout(int id)
{
	Layout *l = findLayout(id);
	QMainWindow *m = mainWindow();
	if (!l || !m || l->state.isEmpty())
		return false;
	state().undoState = m->saveState();
	bool ok = m->restoreState(l->state);
	obs_log(LOG_INFO, "applied layout \"%s\" (%s)", l->name.toUtf8().constData(),
		ok ? "ok" : "restore reported failure");
	return ok;
}

bool undoLayout()
{
	QMainWindow *m = mainWindow();
	if (!m || state().undoState.isEmpty())
		return false;
	QByteArray back = m->saveState();
	bool ok = m->restoreState(state().undoState);
	state().undoState = back; /* undo the undo works too */
	return ok;
}

void initAfterLoad()
{
	applyNesting();
	applySearchBars();
	refreshNow();
}

void shutdown()
{
	shuttingDown = true;
	if (refreshTimer)
		refreshTimer->stop();
}

} // namespace panels
} // namespace dockx
