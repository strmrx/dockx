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
#include <QDockWidget>
#include <QEvent>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QSlider>
#include <QTimer>

#include <algorithm>

namespace dockx {

const char *PRESET_COLORS[8] = {"#e5534b", "#f0883e", "#e3b341", "#57ab5a",
				"#39c5cf", "#539bf5", "#986ee2", "#e275ad"};
const char *PRESET_COLOR_NAMES[8] = {"Red", "Orange", "Yellow", "Green",
				     "Teal", "Blue", "Purple", "Pink"};

QIcon colorDot(const QColor &c)
{
	/* drawn large and scaled down so the dot stays crisp at any icon size */
	QPixmap pm(32, 32);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setBrush(c);
	p.setPen(Qt::NoPen);
	p.drawEllipse(6, 6, 20, 20);
	p.end();
	return QIcon(pm);
}

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
		locks::registerHotkeys();
		return;
	}

	obs_data_set_default_bool(d, "nesting", true);
	obs_data_set_default_bool(d, "scene_search", true);
	obs_data_set_default_bool(d, "source_search", true);
	obs_data_set_default_bool(d, "scene_colors", true);
	obs_data_set_default_bool(d, "dock_colors", true);
	obs_data_set_default_bool(d, "filter_hotkeys", true);
	obs_data_set_default_bool(d, "folder_new_button", true);
	obs_data_set_default_bool(d, "folder_nesting", true);
	obs_data_set_default_bool(d, "folder_sources", true);
	obs_data_set_default_bool(d, "scene_thumbs", true);
	obs_data_set_default_bool(d, "align_tools", false);
	obs_data_set_default_bool(d, "auto_rescue", true);
	obs_data_set_default_int(d, "next_id", 1);
	obs_data_set_default_int(d, "next_source_dock_id", 1);
	obs_data_set_default_int(d, "next_loadout_id", 1);

	g_state.nesting = obs_data_get_bool(d, "nesting");
	g_state.sceneSearch = obs_data_get_bool(d, "scene_search");
	g_state.sourceSearch = obs_data_get_bool(d, "source_search");
	g_state.sceneColors = obs_data_get_bool(d, "scene_colors");
	g_state.dockColors = obs_data_get_bool(d, "dock_colors");
	g_state.filterHotkeys = obs_data_get_bool(d, "filter_hotkeys");
	g_state.folderNewButton = obs_data_get_bool(d, "folder_new_button");
	g_state.folderNesting = obs_data_get_bool(d, "folder_nesting");
	g_state.folderGridMode = obs_data_get_bool(d, "folder_grid_mode");
	g_state.folderDockIntroduced = obs_data_get_bool(d, "folder_dock_introduced");
	g_state.dragHintsShown = obs_data_get_bool(d, "drag_hints_shown");
	g_state.folderSources = obs_data_get_bool(d, "folder_sources");
	g_state.missingAutoPop = obs_data_get_bool(d, "missing_auto_pop");
	g_state.sceneThumbs = obs_data_get_bool(d, "scene_thumbs");
	g_state.alignTools = obs_data_get_bool(d, "align_tools");
	g_state.autoRescue = obs_data_get_bool(d, "auto_rescue");
	g_state.nextLoadoutId = (int)obs_data_get_int(d, "next_loadout_id");
	obs_data_array_t *louts = obs_data_get_array(d, "loadouts");
	if (louts) {
		const size_t ln = obs_data_array_count(louts);
		for (size_t i = 0; i < ln; i++) {
			obs_data_t *e = obs_data_array_item(louts, i);
			g_state.loadouts.push_back(loadouts::fromData(e));
			obs_data_release(e);
		}
		obs_data_array_release(louts);
	}
	g_state.hasLoadoutUndo = obs_data_get_bool(d, "has_loadout_undo");
	if (g_state.hasLoadoutUndo) {
		obs_data_t *u = obs_data_get_obj(d, "loadout_undo");
		if (u) {
			g_state.loadoutUndo = loadouts::fromData(u);
			obs_data_release(u);
		} else {
			g_state.hasLoadoutUndo = false;
		}
	}
	g_state.sepSize = (int)obs_data_get_int(d, "sep_size");
	g_state.sepColor = QString::fromUtf8(obs_data_get_string(d, "sep_color"));
	g_state.mixerOrder = QString::fromUtf8(obs_data_get_string(d, "mixer_order"))
				     .split('\n', Qt::SkipEmptyParts);
	g_state.nextId = (int)obs_data_get_int(d, "next_id");
	g_state.nextSourceDockId = (int)obs_data_get_int(d, "next_source_dock_id");

	obs_data_array_t *sdocks = obs_data_get_array(d, "source_docks");
	if (sdocks) {
		const size_t n = obs_data_array_count(sdocks);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(sdocks, i);
			SourceDockEntry e;
			e.id = (int)obs_data_get_int(o, "id");
			e.kind = (int)obs_data_get_int(o, "kind");
			e.sourceName = QString::fromUtf8(obs_data_get_string(o, "name"));
			if (e.id > 0)
				g_state.sourceDocks.push_back(e);
			obs_data_release(o);
		}
		obs_data_array_release(sdocks);
	}

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

	obs_data_t *dockColors = obs_data_get_obj(d, "dock_color_map");
	if (dockColors) {
		for (obs_data_item_t *item = obs_data_first(dockColors); item;
		     obs_data_item_next(&item)) {
			const char *key = obs_data_item_get_name(item);
			const char *hex = obs_data_item_get_string(item);
			if (key && hex && *hex)
				g_state.dockColorMap[QString::fromUtf8(key)] = QString::fromUtf8(hex);
		}
		obs_data_release(dockColors);
	}

	obs_data_t *autoRules = obs_data_get_obj(d, "scene_layouts");
	if (autoRules) {
		for (obs_data_item_t *item = obs_data_first(autoRules); item;
		     obs_data_item_next(&item)) {
			const char *scene = obs_data_item_get_name(item);
			long long id = obs_data_item_get_int(item);
			if (scene && id > 0)
				g_state.sceneLayouts[QString::fromUtf8(scene)] = (int)id;
		}
		obs_data_release(autoRules);
	}

	obs_data_t *folders = obs_data_get_obj(d, "folders");
	if (folders) {
		for (obs_data_item_t *item = obs_data_first(folders); item;
		     obs_data_item_next(&item)) {
			const char *coll = obs_data_item_get_name(item);
			obs_data_t *fo = obs_data_item_get_obj(item);
			if (!coll || !fo) {
				if (fo)
					obs_data_release(fo);
				continue;
			}
			FolderData fd;
			fd.order = QString::fromUtf8(obs_data_get_string(fo, "order"))
					   .split('\n', Qt::SkipEmptyParts);
			const QStringList col =
				QString::fromUtf8(obs_data_get_string(fo, "collapsed"))
					.split('\n', Qt::SkipEmptyParts);
			fd.collapsed = QSet<QString>(col.begin(), col.end());
			obs_data_t *as = obs_data_get_obj(fo, "assign");
			if (as) {
				for (obs_data_item_t *a = obs_data_first(as); a;
				     obs_data_item_next(&a)) {
					const char *uuid = obs_data_item_get_name(a);
					const char *folder = obs_data_item_get_string(a);
					if (uuid && folder && *folder)
						fd.assign[QString::fromUtf8(uuid)] =
							QString::fromUtf8(folder);
				}
				obs_data_release(as);
			}
			obs_data_t *fc = obs_data_get_obj(fo, "colors");
			if (fc) {
				for (obs_data_item_t *a = obs_data_first(fc); a;
				     obs_data_item_next(&a)) {
					const char *fname = obs_data_item_get_name(a);
					const char *hex = obs_data_item_get_string(a);
					if (fname && hex && *hex)
						fd.colors[QString::fromUtf8(fname)] =
							QString::fromUtf8(hex);
				}
				obs_data_release(fc);
			}
			g_state.folders[QString::fromUtf8(coll)] = fd;
			obs_data_release(fo);
		}
		obs_data_release(folders);
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
	g_state.hardLock = obs_data_get_bool(d, "hard_lock");
	g_state.lockPoint = QByteArray::fromBase64(obs_data_get_string(d, "lock_point"));
	locks::registerHotkeys();
	locks::loadHotkeys(d);

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
	obs_data_set_bool(d, "dock_colors", g_state.dockColors);
	obs_data_set_bool(d, "filter_hotkeys", g_state.filterHotkeys);
	obs_data_set_bool(d, "folder_new_button", g_state.folderNewButton);
	obs_data_set_bool(d, "folder_nesting", g_state.folderNesting);
	obs_data_set_bool(d, "folder_grid_mode", g_state.folderGridMode);
	obs_data_set_bool(d, "folder_dock_introduced", g_state.folderDockIntroduced);
	obs_data_set_bool(d, "drag_hints_shown", g_state.dragHintsShown);
	obs_data_set_bool(d, "folder_sources", g_state.folderSources);
	obs_data_set_bool(d, "missing_auto_pop", g_state.missingAutoPop);
	obs_data_set_bool(d, "scene_thumbs", g_state.sceneThumbs);
	obs_data_set_bool(d, "align_tools", g_state.alignTools);
	obs_data_set_bool(d, "auto_rescue", g_state.autoRescue);
	obs_data_set_int(d, "next_loadout_id", g_state.nextLoadoutId);
	obs_data_array_t *louts = obs_data_array_create();
	for (const SourceLoadout &l : g_state.loadouts) {
		obs_data_t *e = loadouts::toData(l);
		obs_data_array_push_back(louts, e);
		obs_data_release(e);
	}
	obs_data_set_array(d, "loadouts", louts);
	obs_data_array_release(louts);
	obs_data_set_bool(d, "has_loadout_undo", g_state.hasLoadoutUndo);
	if (g_state.hasLoadoutUndo) {
		obs_data_t *u = loadouts::toData(g_state.loadoutUndo);
		obs_data_set_obj(d, "loadout_undo", u);
		obs_data_release(u);
	}
	obs_data_set_int(d, "sep_size", g_state.sepSize);
	obs_data_set_string(d, "sep_color", g_state.sepColor.toUtf8().constData());
	obs_data_set_string(d, "mixer_order",
			    g_state.mixerOrder.join(QChar('\n')).toUtf8().constData());
	obs_data_set_int(d, "next_id", g_state.nextId);
	obs_data_set_int(d, "next_source_dock_id", g_state.nextSourceDockId);

	obs_data_array_t *sdocks = obs_data_array_create();
	for (const SourceDockEntry &e : g_state.sourceDocks) {
		obs_data_t *o = obs_data_create();
		obs_data_set_int(o, "id", e.id);
		obs_data_set_int(o, "kind", e.kind);
		obs_data_set_string(o, "name", e.sourceName.toUtf8().constData());
		obs_data_array_push_back(sdocks, o);
		obs_data_release(o);
	}
	obs_data_set_array(d, "source_docks", sdocks);
	obs_data_array_release(sdocks);

	obs_data_t *colors = obs_data_create();
	for (auto it = g_state.colors.constBegin(); it != g_state.colors.constEnd(); ++it)
		obs_data_set_string(colors, it.key().toUtf8().constData(),
				    it.value().toUtf8().constData());
	obs_data_set_obj(d, "colors", colors);
	obs_data_release(colors);

	obs_data_t *dockColors = obs_data_create();
	for (auto it = g_state.dockColorMap.constBegin(); it != g_state.dockColorMap.constEnd();
	     ++it)
		obs_data_set_string(dockColors, it.key().toUtf8().constData(),
				    it.value().toUtf8().constData());
	obs_data_set_obj(d, "dock_color_map", dockColors);
	obs_data_release(dockColors);

	obs_data_t *autoRules = obs_data_create();
	for (auto it = g_state.sceneLayouts.constBegin(); it != g_state.sceneLayouts.constEnd();
	     ++it)
		obs_data_set_int(autoRules, it.key().toUtf8().constData(), it.value());
	obs_data_set_obj(d, "scene_layouts", autoRules);
	obs_data_release(autoRules);

	obs_data_t *folders = obs_data_create();
	for (auto it = g_state.folders.constBegin(); it != g_state.folders.constEnd(); ++it) {
		const FolderData &fd = it.value();
		if (fd.assign.isEmpty() && fd.order.isEmpty())
			continue;
		obs_data_t *fo = obs_data_create();
		obs_data_set_string(fo, "order",
				    fd.order.join(QChar('\n')).toUtf8().constData());
		obs_data_set_string(fo, "collapsed",
				    QStringList(fd.collapsed.begin(), fd.collapsed.end())
					    .join(QChar('\n'))
					    .toUtf8()
					    .constData());
		obs_data_t *as = obs_data_create();
		for (auto a = fd.assign.constBegin(); a != fd.assign.constEnd(); ++a)
			obs_data_set_string(as, a.key().toUtf8().constData(),
					    a.value().toUtf8().constData());
		obs_data_set_obj(fo, "assign", as);
		obs_data_release(as);
		obs_data_t *fc = obs_data_create();
		for (auto a = fd.colors.constBegin(); a != fd.colors.constEnd(); ++a)
			obs_data_set_string(fc, a.key().toUtf8().constData(),
					    a.value().toUtf8().constData());
		obs_data_set_obj(fo, "colors", fc);
		obs_data_release(fc);
		obs_data_set_obj(folders, it.key().toUtf8().constData(), fo);
		obs_data_release(fo);
	}
	obs_data_set_obj(d, "folders", folders);
	obs_data_release(folders);

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

	obs_data_set_bool(d, "hard_lock", g_state.hardLock);
	obs_data_set_string(d, "lock_point", g_state.lockPoint.toBase64().constData());
	locks::saveHotkeys(d);

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

QListWidget *nativeSceneList()
{
	return sceneList();
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

/* ---- dock borders & separators ---- */

/* every stylesheet we put on a dock starts with this, so we never
   clobber a stylesheet somebody else (theme, other plugin) set */
static const char *DOCK_QSS_MARK = "/*dockx*/";
static const char *SEP_MARK_BEGIN = "/*dockx-sep*/";
static const char *SEP_MARK_END = "/*dockx-sep-end*/";

static QString dockKey(const QDockWidget *dock)
{
	const QString obj = dock->objectName();
	return obj.isEmpty() ? dock->windowTitle() : obj;
}

/* black or white, whichever reads best on this background */
static QString contrastText(const QColor &c)
{
	const double lum = 0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue();
	return lum > 150 ? QStringLiteral("#000000") : QStringLiteral("#ffffff");
}

QList<DockInfo> listDocks()
{
	QList<DockInfo> out;
	QMainWindow *m = mainWindow();
	if (!m)
		return out;
	const QList<QDockWidget *> docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks) {
		if (d->windowTitle().isEmpty())
			continue;
		out.append({dockKey(d), d->windowTitle()});
	}
	std::sort(out.begin(), out.end(), [](const DockInfo &a, const DockInfo &b) {
		return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
	});
	return out;
}

static void applyDockColorsNow()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	const QList<QDockWidget *> docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks) {
		const QString hex =
			state().dockColors ? state().dockColorMap.value(dockKey(d)) : QString();
		const QString current = d->styleSheet();
		if (hex.isEmpty()) {
			/* only remove styles we put there ourselves */
			if (current.startsWith(DOCK_QSS_MARK))
				d->setStyleSheet(QString());
			continue;
		}
		/* if someone else styled this dock, leave it alone */
		if (!current.isEmpty() && !current.startsWith(DOCK_QSS_MARK))
			continue;
		const QColor c(hex);
		const QString qss = QString("%1 QDockWidget { border: 2px solid %2; color: %3; } "
					    "QDockWidget::title { background: %2; }")
					    .arg(DOCK_QSS_MARK, hex, contrastText(c));
		if (current != qss)
			d->setStyleSheet(qss);
	}
}

void applyDockColors()
{
	applyDockColorsNow();
}

void applySeparators()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	QString qss = m->styleSheet();
	/* strip our previous block, keep everything anyone else added */
	int b = qss.indexOf(SEP_MARK_BEGIN);
	if (b >= 0) {
		int e = qss.indexOf(SEP_MARK_END);
		if (e >= 0)
			qss.remove(b, e + (int)strlen(SEP_MARK_END) - b);
		else
			qss.truncate(b);
	}
	if (state().sepSize > 0 || !state().sepColor.isEmpty()) {
		QString block = QString(SEP_MARK_BEGIN) + " QMainWindow::separator {";
		if (state().sepSize > 0)
			block += QString(" width: %1px; height: %1px;").arg(state().sepSize);
		if (!state().sepColor.isEmpty())
			block += QString(" background: %1;").arg(state().sepColor);
		block += " } ";
		if (!state().sepColor.isEmpty())
			block += QString("QMainWindow::separator:hover { background: %1; } ")
					 .arg(QColor(state().sepColor).lighter(130).name());
		qss += block + SEP_MARK_END;
	}
	m->setStyleSheet(qss);
}

/* browser docks and plugin docks appear after we first load; recolor
   whenever a new child shows up under the main window */
class DockWatcher : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		if (ev->type() == QEvent::ChildAdded)
			refreshSoon();
		return QObject::eventFilter(obj, ev);
	}
};

static QPointer<DockWatcher> dockWatcher;

static void watchDocks()
{
	QMainWindow *m = mainWindow();
	if (!m || dockWatcher)
		return;
	dockWatcher = new DockWatcher(m);
	m->installEventFilter(dockWatcher);
}

static void applySceneColorsNow()
{
	QListWidget *list = sceneList();
	if (!list)
		return;
	applying = true;
	/* grid mode: a dot icon stacks above the name and doubles the tile height,
	   so color the whole tile instead (contrast aware text) */
	const bool grid = list->viewMode() == QListView::IconMode;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *it = list->item(i);
		const QString hex = state().sceneColors ? state().colors.value(it->text())
						       : QString();
		if (!hex.isEmpty()) {
			const QColor c(hex);
			if (grid) {
				it->setIcon(QIcon());
				it->setBackground(QBrush(c));
				it->setForeground(QBrush(
					c.lightness() > 140 ? Qt::black : Qt::white));
			} else {
				it->setData(Qt::BackgroundRole, QVariant());
				it->setForeground(QBrush(c));
				it->setIcon(colorDot(c));
			}
		} else {
			it->setData(Qt::ForegroundRole, QVariant());
			it->setData(Qt::BackgroundRole, QVariant());
			it->setIcon(QIcon());
		}
	}
	applying = false;
}

/* OBS's Grid Mode toggle emits no frontend event; watch the scenes list for the
   view mode flipping and re-decorate (dots in list mode, full tiles in grid) */
class GridWatcher : public QObject {
public:
	using QObject::QObject;
	int lastMode = -1;

protected:
	bool eventFilter(QObject *obj, QEvent *ev) override
	{
		const QEvent::Type t = ev->type();
		if (t == QEvent::LayoutRequest || t == QEvent::Resize ||
		    t == QEvent::Show) {
			QListWidget *list = qobject_cast<QListWidget *>(obj);
			if (list && (int)list->viewMode() != lastMode) {
				lastMode = (int)list->viewMode();
				refreshSoon();
			}
		}
		return false;
	}
};

/* OBS rebuilds both lists behind our back (scene switches, renames, collection
   changes). Watch the current models and re-decorate shortly after they move. */
static void watchModels()
{
	QListWidget *scenes = sceneList();
	if (scenes && !scenes->property("dockx_grid_watch").toBool()) {
		scenes->setProperty("dockx_grid_watch", true);
		GridWatcher *gw = new GridWatcher(scenes);
		gw->lastMode = (int)scenes->viewMode();
		scenes->installEventFilter(gw);
	}
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

/* ================= audio mixer order ================= */

static QWidget *volumeContainer(const char *name)
{
	QMainWindow *m = mainWindow();
	return m ? m->findChild<QWidget *>(name) : nullptr;
}

/* a VolControl's name label: the first label that is not the dB readout */
static QString volControlName(QWidget *w)
{
	const QList<QLabel *> labels = w->findChildren<QLabel *>();
	for (QLabel *l : labels) {
		const QString t = l->text();
		if (!t.isEmpty() && !t.contains(QStringLiteral("dB")))
			return t;
	}
	return QString();
}

static bool isVolControl(QWidget *w)
{
	if (!w)
		return false;
	if (qstrcmp(w->metaObject()->className(), "VolControl") == 0)
		return true;
	/* fallback: any mixer row has a fader slider in it */
	return w->findChild<QSlider *>() != nullptr;
}

QStringList mixerSourceNames()
{
	QStringList out;
	for (const char *cname : {"hVolumeWidgets", "vVolumeWidgets"}) {
		QWidget *c = volumeContainer(cname);
		QBoxLayout *lay = c ? qobject_cast<QBoxLayout *>(c->layout()) : nullptr;
		if (!lay)
			continue;
		for (int i = 0; i < lay->count(); i++) {
			QWidget *w = lay->itemAt(i)->widget();
			if (isVolControl(w)) {
				const QString n = volControlName(w);
				if (!n.isEmpty())
					out << n;
			}
		}
		if (!out.isEmpty())
			break; /* only one container is populated at a time */
	}
	return out;
}

void applyMixerOrder()
{
	if (state().mixerOrder.isEmpty() || shuttingDown)
		return;
	const QStringList &order = state().mixerOrder;
	for (const char *cname : {"hVolumeWidgets", "vVolumeWidgets"}) {
		QWidget *c = volumeContainer(cname);
		QBoxLayout *lay = c ? qobject_cast<QBoxLayout *>(c->layout()) : nullptr;
		if (!lay)
			continue;
		struct Row {
			QWidget *w;
			int rank;
		};
		std::vector<Row> rows;
		for (int i = 0; i < lay->count(); i++) {
			QWidget *w = lay->itemAt(i)->widget();
			if (!isVolControl(w))
				continue;
			const int r = (int)order.indexOf(volControlName(w));
			rows.push_back({w, r < 0 ? (1 << 30) : r});
		}
		if (rows.size() < 2)
			continue;
		std::vector<Row> sorted = rows;
		std::stable_sort(sorted.begin(), sorted.end(),
				 [](const Row &a, const Row &b) { return a.rank < b.rank; });
		bool changed = false;
		for (size_t i = 0; i < rows.size(); i++)
			if (rows[i].w != sorted[i].w)
				changed = true;
		if (!changed)
			continue;
		applying = true;
		for (const Row &r : sorted)
			lay->removeWidget(r.w);
		for (size_t i = 0; i < sorted.size(); i++)
			lay->insertWidget((int)i, sorted[i].w);
		applying = false;
	}
}

/* the mixer rebuilds its rows on scene/source changes; watch for new rows */
class MixerWatcher : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *, QEvent *ev) override
	{
		if (ev->type() == QEvent::ChildAdded && !applying)
			refreshSoon();
		return false;
	}
};

static void watchMixer()
{
	for (const char *cname : {"hVolumeWidgets", "vVolumeWidgets"}) {
		QWidget *c = volumeContainer(cname);
		if (c && !c->property("dockx_mixer_watch").toBool()) {
			c->setProperty("dockx_mixer_watch", true);
			c->installEventFilter(new MixerWatcher(c));
		}
	}
}

static void refreshNow()
{
	if (shuttingDown)
		return;
	watchModels();
	watchMixer();
	applySceneColorsNow();
	applyDockColorsNow();
	applyMixerOrder();
	filterScenes();
	filterSources();
	folders::rebuildSoon(); /* scene renames/colors show up in the folder tree too */
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

void autoSceneLayout()
{
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (!scene)
		return;
	const QString name = QString::fromUtf8(obs_source_get_name(scene));
	obs_source_release(scene);
	const int id = state().sceneLayouts.value(name, 0);
	if (!id || !findLayout(id))
		return;
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	/* never rebuild docks from inside the frontend event callback */
	QMetaObject::invokeMethod(
		m, [id]() { applyLayout(id); }, Qt::QueuedConnection);
}

bool applyLayout(int id)
{
	Layout *l = findLayout(id);
	QMainWindow *m = mainWindow();
	if (!l || !m || l->state.isEmpty())
		return false;
	state().undoState = m->saveState();
	bool ok = m->restoreState(l->state);
	locks::applyHardLock(); /* restoreState can re-show docks; reassert the freeze */
	monitors::validateVisible(); /* a layout saved for more screens can't strand a dock */
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
	applySeparators();
	watchDocks();
	refreshNow();
	locks::applyHardLock(); /* honor a saved dock freeze on startup */
}

void shutdown()
{
	shuttingDown = true;
	if (refreshTimer)
		refreshTimer->stop();
}

} // namespace panels
} // namespace dockx
