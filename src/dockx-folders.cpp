/*
DockX for OBS Studio (by StrmrX) -- the Scene Folders dock.
GPL v2, see plugin-main.cpp for the full notice.

A collapsible folder tree over the scene list, in its own dock. OBS's native
Scenes panel cannot be safely restructured, so we present our own view and
drive OBS through the frontend API. Folder assignments are keyed by scene
UUID (renames never lose a folder) and stored per scene collection.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAction>
#include <QBrush>
#include <QColorDialog>
#include <QDockWidget>
#include <QDropEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QShortcut>
#include <QMessageBox>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <vector>

namespace dockx {
namespace folders {

static QPointer<QTreeWidget> g_tree;
static QPointer<QLineEdit> g_search;
static QPointer<QToolButton> g_newBtn;
static QPointer<QTimer> g_timer;
static bool g_applying = false;
static bool g_shutdown = false;

static const char *ROLE_TYPE_FOLDER = "f";
static const char *ROLE_TYPE_SCENE = "s";

static QString currentCollection()
{
	char *c = obs_frontend_get_current_scene_collection();
	if (!c)
		return QString();
	QString out = QString::fromUtf8(c);
	bfree(c);
	return out;
}

static FolderData &data()
{
	return state().folders[currentCollection()];
}

static bool isFolder(const QTreeWidgetItem *it)
{
	return it && it->data(0, Qt::UserRole).toString() == ROLE_TYPE_FOLDER;
}

static bool isScene(const QTreeWidgetItem *it)
{
	return it && it->data(0, Qt::UserRole).toString() == ROLE_TYPE_SCENE;
}

static QString itemKey(const QTreeWidgetItem *it)
{
	return it->data(0, Qt::UserRole + 1).toString();
}

static void applySearch()
{
	if (!g_tree)
		return;
	const QString q = g_search ? g_search->text().trimmed() : QString();
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = g_tree->topLevelItem(i);
		if (isFolder(it)) {
			bool anyVisible = false;
			for (int j = 0; j < it->childCount(); j++) {
				QTreeWidgetItem *c = it->child(j);
				const bool hide = !q.isEmpty() &&
						  !c->text(0).contains(q, Qt::CaseInsensitive);
				c->setHidden(hide);
				if (!hide)
					anyVisible = true;
			}
			const bool nameHit =
				!q.isEmpty() && itemKey(it).contains(q, Qt::CaseInsensitive);
			it->setHidden(!q.isEmpty() && !anyVisible && !nameHit);
			if (!q.isEmpty() && anyVisible)
				it->setExpanded(true); /* searching peeks into folders */
		} else {
			it->setHidden(!q.isEmpty() &&
				      !it->text(0).contains(q, Qt::CaseInsensitive));
		}
	}
}

static QTreeWidgetItem *findByKey(const QString &key)
{
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = g_tree->topLevelItem(i);
		if (itemKey(it) == key)
			return it;
		for (int j = 0; j < it->childCount(); j++)
			if (itemKey(it->child(j)) == key)
				return it->child(j);
	}
	return nullptr;
}

static void rebuildNow()
{
	if (g_shutdown || !g_tree)
		return;
	g_applying = true;
	/* clearing resets the scroll; remember what was at the top so a rebuild
	   never yanks the list around under the user */
	QString topKey;
	if (QTreeWidgetItem *top = g_tree->itemAt(4, 4))
		topKey = itemKey(top);
	g_tree->clear();

	FolderData &fd = data();

	struct SceneRow {
		QString uuid;
		QString name;
	};
	std::vector<SceneRow> scenes;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_source_t *s = list.sources.array[i];
		const char *uuid = obs_source_get_uuid(s);
		const char *name = obs_source_get_name(s);
		if (uuid && name)
			scenes.push_back({QString::fromUtf8(uuid), QString::fromUtf8(name)});
	}
	obs_frontend_source_list_free(&list);

	/* forget assignments for scenes that no longer exist */
	QSet<QString> alive;
	for (const SceneRow &s : scenes)
		alive.insert(s.uuid);
	for (auto it = fd.assign.begin(); it != fd.assign.end();) {
		if (!alive.contains(it.key()))
			it = fd.assign.erase(it);
		else
			++it;
	}
	/* any folder referenced by an assignment must exist in the order list */
	for (const QString &fname : fd.assign)
		if (!fd.order.contains(fname))
			fd.order.append(fname);

	QString curUuid;
	obs_source_t *cur = obs_frontend_preview_program_mode_active()
				    ? obs_frontend_get_current_preview_scene()
				    : obs_frontend_get_current_scene();
	if (cur) {
		const char *u = obs_source_get_uuid(cur);
		if (u)
			curUuid = QString::fromUtf8(u);
		obs_source_release(cur);
	}

	const QIcon folderIcon = g_tree->style()->standardIcon(QStyle::SP_DirIcon);
	QHash<QString, QTreeWidgetItem *> folderItems;
	for (const QString &fname : fd.order) {
		QTreeWidgetItem *fi = new QTreeWidgetItem(g_tree);
		fi->setData(0, Qt::UserRole, ROLE_TYPE_FOLDER);
		fi->setData(0, Qt::UserRole + 1, fname);
		fi->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled |
			     Qt::ItemIsDropEnabled);
		fi->setIcon(0, folderIcon);
		QFont ff = fi->font(0);
		ff.setBold(true);
		fi->setFont(0, ff);
		folderItems[fname] = fi;
	}

	QTreeWidgetItem *curItem = nullptr;
	for (const SceneRow &s : scenes) {
		QTreeWidgetItem *parent = folderItems.value(fd.assign.value(s.uuid), nullptr);
		QTreeWidgetItem *si = parent ? new QTreeWidgetItem(parent)
					     : new QTreeWidgetItem(g_tree);
		si->setText(0, s.name);
		si->setData(0, Qt::UserRole, ROLE_TYPE_SCENE);
		si->setData(0, Qt::UserRole + 1, s.uuid);
		si->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
		const QString hex = state().sceneColors ? state().colors.value(s.name)
						       : QString();
		if (!hex.isEmpty()) {
			si->setForeground(0, QBrush(QColor(hex)));
			si->setIcon(0, colorDot(QColor(hex)));
		}
		if (s.uuid == curUuid) {
			QFont f = si->font(0);
			f.setBold(true);
			si->setFont(0, f);
			si->setSelected(true);
			curItem = si;
		}
	}

	for (auto it = folderItems.constBegin(); it != folderItems.constEnd(); ++it) {
		QTreeWidgetItem *fi = it.value();
		fi->setText(0, QString("%1  (%2)").arg(it.key()).arg(fi->childCount()));
		fi->setExpanded(!fd.collapsed.contains(it.key()));
	}

	/* put the list back where it was before the rebuild */
	if (!topKey.isEmpty()) {
		if (QTreeWidgetItem *top = findByKey(topKey))
			g_tree->scrollToItem(top, QAbstractItemView::PositionAtTop);
	}

	/* always show where you are: peek into the folder holding the live scene,
	   but only scroll if it is actually off screen (native panel behavior; a
	   collapsed folder stays collapsed next rebuild) */
	if (curItem) {
		if (curItem->parent())
			curItem->parent()->setExpanded(true);
		const QRect r = g_tree->visualItemRect(curItem);
		const int viewH = g_tree->viewport()->height();
		if (r.height() == 0 || r.top() < 0 || r.bottom() > viewH)
			g_tree->scrollToItem(curItem);
	}

	g_applying = false;
	applySearch();
}

void rebuildSoon()
{
	if (g_shutdown || !g_tree)
		return;
	if (!g_timer) {
		g_timer = new QTimer(g_tree);
		g_timer->setSingleShot(true);
		g_timer->setInterval(150);
		QObject::connect(g_timer, &QTimer::timeout, []() { rebuildNow(); });
	}
	g_timer->start();
}

/* after a drag: un-nest any folder that landed inside a folder, then read the
   tree back into state as the new truth */
static void persistFromTree()
{
	if (!g_tree || g_applying)
		return;
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = g_tree->topLevelItem(i);
		for (int j = 0; j < it->childCount(); j++) {
			QTreeWidgetItem *c = it->child(j);
			if (isFolder(c)) {
				it->takeChild(j);
				g_tree->addTopLevelItem(c);
				j--;
			}
		}
	}
	FolderData &fd = data();
	fd.assign.clear();
	QStringList newOrder;
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = g_tree->topLevelItem(i);
		if (!isFolder(it))
			continue;
		const QString fname = itemKey(it);
		newOrder << fname;
		for (int j = 0; j < it->childCount(); j++) {
			QTreeWidgetItem *c = it->child(j);
			if (isScene(c))
				fd.assign[itemKey(c)] = fname;
		}
	}
	fd.order = newOrder;
	stateSave();
	rebuildSoon();
}

class FolderTree : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;

protected:
	void dropEvent(QDropEvent *e) override
	{
		QTreeWidget::dropEvent(e);
		persistFromTree();
	}
};

/* ---- folder management (footer toolbar + context menu) ---- */

static void newFolderPrompt(QWidget *parent, const QString &assignSceneUuid = QString())
{
	bool ok = false;
	QString name = QInputDialog::getText(parent, "New folder", "Folder name:",
					     QLineEdit::Normal, QString(), &ok)
			       .trimmed();
	if (!ok || name.isEmpty())
		return;
	FolderData &fd = data();
	if (fd.order.contains(name)) {
		QMessageBox::information(parent, "DockX",
					 "A folder with that name already exists.");
		return;
	}
	fd.order.append(name);
	if (!assignSceneUuid.isEmpty())
		fd.assign[assignSceneUuid] = name;
	stateSave();
	rebuildNow();
}

static void renameFolderPrompt(QWidget *parent, const QString &oldName)
{
	bool ok = false;
	QString name = QInputDialog::getText(parent, "Rename folder", "New name:",
					     QLineEdit::Normal, oldName, &ok)
			       .trimmed();
	if (!ok || name.isEmpty() || name == oldName)
		return;
	FolderData &fd = data();
	if (fd.order.contains(name)) {
		QMessageBox::information(parent, "DockX",
					 "A folder with that name already exists.");
		return;
	}
	for (int i = 0; i < fd.order.size(); i++)
		if (fd.order[i] == oldName)
			fd.order[i] = name;
	for (auto a = fd.assign.begin(); a != fd.assign.end(); ++a)
		if (a.value() == oldName)
			a.value() = name;
	if (fd.collapsed.remove(oldName))
		fd.collapsed.insert(name);
	stateSave();
	rebuildNow();
}

static void deleteFolderPrompt(QWidget *parent, const QString &name)
{
	auto answer = QMessageBox::question(
		parent, "Delete folder",
		QString("Delete \"%1\"? The scenes inside go back to the main list. "
			"No scene is deleted.")
			.arg(name));
	if (answer != QMessageBox::Yes)
		return;
	FolderData &fd = data();
	fd.order.removeAll(name);
	fd.collapsed.remove(name);
	for (auto a = fd.assign.begin(); a != fd.assign.end();) {
		if (a.value() == name)
			a = fd.assign.erase(a);
		else
			++a;
	}
	stateSave();
	rebuildNow();
}

static void setAllExpanded(bool on)
{
	if (!g_tree)
		return;
	for (int i = 0; i < g_tree->topLevelItemCount(); i++) {
		QTreeWidgetItem *it = g_tree->topLevelItem(i);
		if (isFolder(it))
			it->setExpanded(on);
	}
}

/* ---- scene actions (context menu) ---- */

static obs_source_t *sceneByUuid(const QString &uuid)
{
	return obs_get_source_by_uuid(uuid.toUtf8().constData());
}

static QString uniqueSceneName(const QString &base)
{
	QString name = base;
	int n = 2;
	for (;;) {
		obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData());
		if (!clash)
			return name;
		obs_source_release(clash);
		name = QString("%1 %2").arg(base).arg(n++);
	}
}

static void setSceneColor(const QString &name, const QString &hex)
{
	if (hex.isEmpty())
		state().colors.remove(name);
	else
		state().colors[name] = hex;
	stateSave();
	panels::refreshSoon();
	rebuildSoon();
}

static void renameScenePrompt(const QString &uuid, const QString &name)
{
	bool ok = false;
	QString newName = QInputDialog::getText(g_tree, "Rename scene", "New name:",
						QLineEdit::Normal, name, &ok)
				  .trimmed();
	if (!ok || newName.isEmpty() || newName == name)
		return;
	if (obs_source_t *clash = obs_get_source_by_name(newName.toUtf8().constData())) {
		obs_source_release(clash);
		QMessageBox::information(g_tree, "DockX",
					 "A scene or source with that name already exists.");
		return;
	}
	obs_source_t *src = sceneByUuid(uuid);
	if (!src)
		return;
	obs_source_set_name(src, newName.toUtf8().constData());
	obs_source_release(src);
	/* color labels are keyed by name; carry the label along */
	if (state().colors.contains(name)) {
		state().colors[newName] = state().colors.take(name);
		stateSave();
		panels::refreshSoon();
	}
}

/* prompts for a name (native behavior), refusing clashes */
static QString promptSceneName(const QString &title, const QString &suggested)
{
	bool ok = false;
	QString name = QInputDialog::getText(g_tree, title, "Scene name:", QLineEdit::Normal,
					     suggested, &ok)
			       .trimmed();
	if (!ok || name.isEmpty())
		return QString();
	if (obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(clash);
		QMessageBox::information(g_tree, "DockX",
					 "A scene or source with that name already exists.");
		return QString();
	}
	return name;
}

static void switchToScene(obs_source_t *src)
{
	if (obs_frontend_preview_program_mode_active())
		obs_frontend_set_current_preview_scene(src);
	else
		obs_frontend_set_current_scene(src);
}

static void addScenePrompt(const QString &folder)
{
	const QString name = promptSceneName("Add scene", uniqueSceneName("Scene"));
	if (name.isEmpty())
		return;
	obs_scene_t *scene = obs_scene_create(name.toUtf8().constData());
	if (!scene)
		return;
	if (!folder.isEmpty()) {
		const char *u = obs_source_get_uuid(obs_scene_get_source(scene));
		if (u) {
			data().assign[QString::fromUtf8(u)] = folder;
			stateSave();
		}
	}
	switchToScene(obs_scene_get_source(scene));
	obs_scene_release(scene);
}

static void duplicateScenePrompt(const QString &uuid, const QString &name)
{
	const QString dupName =
		promptSceneName("Duplicate scene", uniqueSceneName(name + " Copy"));
	if (dupName.isEmpty())
		return;
	obs_source_t *src = sceneByUuid(uuid);
	if (!src)
		return;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (scene) {
		obs_scene_t *dup = obs_scene_duplicate(scene, dupName.toUtf8().constData(),
						       OBS_SCENE_DUP_REFS);
		if (dup) {
			/* the copy lands in the same folder, with the same color */
			obs_source_t *dupSrc = obs_scene_get_source(dup);
			const char *du = dupSrc ? obs_source_get_uuid(dupSrc) : nullptr;
			FolderData &fd = data();
			const QString folder = fd.assign.value(uuid);
			if (du && !folder.isEmpty())
				fd.assign[QString::fromUtf8(du)] = folder;
			const QString hex = state().colors.value(name);
			if (!hex.isEmpty())
				state().colors[dupName] = hex;
			stateSave();
			panels::refreshSoon();
			obs_scene_release(dup);
		}
	}
	obs_source_release(src);
}

/* the scene whose filters were last copied (this OBS run only, like native) */
static QString g_copyFiltersUuid;

/* reorder the scene in the NATIVE list; OBS reads the list order back on save,
   and obs_frontend_get_scenes follows it, so our tree mirrors the move.
   modes: 0 up, 1 down, 2 top, 3 bottom */
static void moveSceneRow(const QString &name, int mode)
{
	QListWidget *list = panels::nativeSceneList();
	if (!list)
		return;
	int row = -1;
	for (int i = 0; i < list->count(); i++) {
		if (list->item(i)->text() == name) {
			row = i;
			break;
		}
	}
	if (row < 0)
		return;
	const int dst = mode == 0 ? row - 1
		      : mode == 1 ? row + 1
		      : mode == 2 ? 0
				  : list->count() - 1;
	if (dst < 0 || dst >= list->count() || dst == row)
		return;
	{
		/* silent move: OBS must not see the take/insert as a scene switch */
		QListWidgetItem *cur = list->currentItem();
		const QSignalBlocker block(list);
		QListWidgetItem *it = list->takeItem(row);
		list->insertItem(dst, it);
		if (cur)
			list->setCurrentItem(cur);
	}
	rebuildSoon();
}

static void setTransitionOverride(const QString &uuid, const QString &transition)
{
	obs_source_t *src = sceneByUuid(uuid);
	if (!src)
		return;
	obs_data_t *priv = obs_source_get_private_settings(src);
	if (transition.isEmpty())
		obs_data_erase(priv, "transition");
	else
		obs_data_set_string(priv, "transition", transition.toUtf8().constData());
	obs_data_release(priv);
	obs_source_release(src);
}

static void removeScenePrompt(const QString &uuid, const QString &name)
{
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	const size_t count = list.sources.num;
	obs_frontend_source_list_free(&list);
	if (count <= 1) {
		QMessageBox::information(g_tree, "DockX", "OBS needs at least one scene.");
		return;
	}
	if (QMessageBox::question(g_tree, "Remove scene",
				  QString("Remove \"%1\" from OBS? This deletes the scene.")
					  .arg(name)) != QMessageBox::Yes)
		return;
	if (obs_source_t *src = sceneByUuid(uuid)) {
		obs_source_remove(src);
		obs_source_release(src);
	}
}

static void showContextMenu(const QPoint &pos)
{
	if (!g_tree)
		return;
	QTreeWidgetItem *it = g_tree->itemAt(pos);
	QMenu menu(g_tree);

	if (isScene(it)) {
		const QString uuid = itemKey(it);
		const QString name = it->text(0);

		/* current per-scene state, read once for the checkmarks */
		bool hasFilters = false, inMultiview = true;
		QString curOverride;
		int curDur = 300;
		if (obs_source_t *src = sceneByUuid(uuid)) {
			hasFilters = obs_source_filter_count(src) > 0;
			obs_data_t *priv = obs_source_get_private_settings(src);
			obs_data_set_default_bool(priv, "show_in_multiview", true);
			obs_data_set_default_int(priv, "transition_duration", 300);
			inMultiview = obs_data_get_bool(priv, "show_in_multiview");
			curOverride =
				QString::fromUtf8(obs_data_get_string(priv, "transition"));
			curDur = (int)obs_data_get_int(priv, "transition_duration");
			obs_data_release(priv);
			obs_source_release(src);
		}

		menu.addAction("Add Scene...", [uuid]() {
			addScenePrompt(data().assign.value(uuid));
		});
		menu.addAction("Duplicate...",
			       [uuid, name]() { duplicateScenePrompt(uuid, name); });
		QAction *copyF = menu.addAction("Copy Filters",
						[uuid]() { g_copyFiltersUuid = uuid; });
		copyF->setEnabled(hasFilters);
		bool canPaste = false;
		if (!g_copyFiltersUuid.isEmpty() && g_copyFiltersUuid != uuid) {
			if (obs_source_t *s = sceneByUuid(g_copyFiltersUuid)) {
				canPaste = true;
				obs_source_release(s);
			}
		}
		QAction *pasteF = menu.addAction("Paste Filters", [uuid]() {
			obs_source_t *from = sceneByUuid(g_copyFiltersUuid);
			obs_source_t *to = sceneByUuid(uuid);
			if (from && to && from != to)
				obs_source_copy_filters(to, from);
			if (from)
				obs_source_release(from);
			if (to)
				obs_source_release(to);
		});
		pasteF->setEnabled(canPaste);

		menu.addSeparator();
		QAction *renA = menu.addAction(
			"Rename...", [uuid, name]() { renameScenePrompt(uuid, name); });
		renA->setShortcut(QKeySequence(Qt::Key_F2));
		renA->setShortcutContext(Qt::WidgetShortcut);
		renA->setShortcutVisibleInContextMenu(true);
		QAction *remA = menu.addAction(
			"Remove", [uuid, name]() { removeScenePrompt(uuid, name); });
		remA->setShortcut(QKeySequence(Qt::Key_Delete));
		remA->setShortcutContext(Qt::WidgetShortcut);
		remA->setShortcutVisibleInContextMenu(true);

		menu.addSeparator();
		QMenu *orderMenu = menu.addMenu("Order");
		orderMenu->addAction("Move Up", [name]() { moveSceneRow(name, 0); });
		orderMenu->addAction("Move Down", [name]() { moveSceneRow(name, 1); });
		orderMenu->addAction("Move to Top", [name]() { moveSceneRow(name, 2); });
		orderMenu->addAction("Move to Bottom", [name]() { moveSceneRow(name, 3); });

		QMenu *moveMenu = menu.addMenu("Move to Folder");
		FolderData &fd = data();
		const QString curFolder = fd.assign.value(uuid);
		for (const QString &fname : fd.order) {
			QAction *a = moveMenu->addAction(fname, [uuid, fname]() {
				data().assign[uuid] = fname;
				stateSave();
				rebuildNow();
			});
			a->setCheckable(true);
			a->setChecked(fname == curFolder);
		}
		if (!fd.order.isEmpty())
			moveMenu->addSeparator();
		QAction *noneA = moveMenu->addAction("No folder", [uuid]() {
			data().assign.remove(uuid);
			stateSave();
			rebuildNow();
		});
		noneA->setCheckable(true);
		noneA->setChecked(curFolder.isEmpty());
		moveMenu->addAction("New folder...",
				    [uuid]() { newFolderPrompt(g_tree, uuid); });

		QMenu *colorMenu = menu.addMenu("Set Color");
		for (int i = 0; i < 8; i++) {
			const QString hex = QString::fromUtf8(PRESET_COLORS[i]);
			colorMenu->addAction(colorDot(QColor(hex)), PRESET_COLOR_NAMES[i],
					     [name, hex]() { setSceneColor(name, hex); });
		}
		colorMenu->addAction("Custom...", [name]() {
			QColor c = QColorDialog::getColor(Qt::white, g_tree, "Pick a color");
			if (c.isValid())
				setSceneColor(name, c.name());
		});
		colorMenu->addAction("No color",
				     [name]() { setSceneColor(name, QString()); });

		menu.addSeparator();
		QMenu *projMenu = menu.addMenu("Open Scene Projector");
		QMenu *fsMenu = projMenu->addMenu("Fullscreen");
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			const QRect g = screens[i]->geometry();
			fsMenu->addAction(QString("Display %1 (%2x%3)")
						  .arg(i + 1)
						  .arg(g.width())
						  .arg(g.height()),
					  [i, name]() {
						  obs_frontend_open_projector(
							  "Scene", i, nullptr,
							  name.toUtf8().constData());
					  });
		}
		projMenu->addAction("Windowed", [name]() {
			obs_frontend_open_projector("Scene", -1, nullptr,
						    name.toUtf8().constData());
		});
		menu.addAction("Save Scene Screenshot", [uuid]() {
			if (obs_source_t *src = sceneByUuid(uuid)) {
				obs_frontend_take_source_screenshot(src);
				obs_source_release(src);
			}
		});

		menu.addSeparator();
		menu.addAction("Filters", [uuid]() {
			if (obs_source_t *src = sceneByUuid(uuid)) {
				obs_frontend_open_source_filters(src);
				obs_source_release(src);
			}
		});
		QMenu *trMenu = menu.addMenu("Transition Override");
		QAction *noneT = trMenu->addAction(
			"None", [uuid]() { setTransitionOverride(uuid, QString()); });
		noneT->setCheckable(true);
		noneT->setChecked(curOverride.isEmpty());
		obs_frontend_source_list tl = {};
		obs_frontend_get_transitions(&tl);
		for (size_t i = 0; i < tl.sources.num; i++) {
			const char *tn = obs_source_get_name(tl.sources.array[i]);
			if (!tn)
				continue;
			const QString tname = QString::fromUtf8(tn);
			QAction *a = trMenu->addAction(tname, [uuid, tname]() {
				setTransitionOverride(uuid, tname);
			});
			a->setCheckable(true);
			a->setChecked(tname == curOverride);
		}
		obs_frontend_source_list_free(&tl);
		trMenu->addSeparator();
		trMenu->addAction(QString("Duration (%1 ms)...").arg(curDur),
				  [uuid, curDur]() {
					  bool ok = false;
					  const int ms = QInputDialog::getInt(
						  g_tree, "Transition duration",
						  "Milliseconds:", curDur, 50, 20000, 50,
						  &ok);
					  if (!ok)
						  return;
					  obs_source_t *s = sceneByUuid(uuid);
					  if (!s)
						  return;
					  obs_data_t *priv =
						  obs_source_get_private_settings(s);
					  obs_data_set_int(priv, "transition_duration", ms);
					  obs_data_release(priv);
					  obs_source_release(s);
				  });
		QAction *mv = menu.addAction("Show in Multiview");
		mv->setCheckable(true);
		mv->setChecked(inMultiview);
		QObject::connect(mv, &QAction::triggered, g_tree, [uuid](bool on) {
			obs_source_t *s = sceneByUuid(uuid);
			if (!s)
				return;
			obs_data_t *priv = obs_source_get_private_settings(s);
			obs_data_set_bool(priv, "show_in_multiview", on);
			obs_data_release(priv);
			obs_source_release(s);
		});
	} else if (isFolder(it)) {
		const QString fname = itemKey(it);
		menu.addAction("Add Scene...", [fname]() { addScenePrompt(fname); });
		menu.addAction("New folder", []() { newFolderPrompt(g_tree); });
		menu.addAction("Rename", [fname]() { renameFolderPrompt(g_tree, fname); });
		menu.addAction("Delete", [fname]() { deleteFolderPrompt(g_tree, fname); });
		menu.addSeparator();
		menu.addAction("Collapse all", []() { setAllExpanded(false); });
		menu.addAction("Expand all", []() { setAllExpanded(true); });
	} else {
		menu.addAction("Add Scene...", []() { addScenePrompt(QString()); });
		menu.addAction("New folder", []() { newFolderPrompt(g_tree); });
		menu.addSeparator();
		menu.addAction("Collapse all", []() { setAllExpanded(false); });
		menu.addAction("Expand all", []() { setAllExpanded(true); });
	}

	menu.exec(g_tree->viewport()->mapToGlobal(pos));
}

void createDock()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *v = new QVBoxLayout(panel);
	v->setContentsMargins(4, 4, 4, 4);
	v->setSpacing(4);

	/* search + the small new folder button share one row (setting can hide the
	   button; everything it does is also in the right click menu) */
	QHBoxLayout *topRow = new QHBoxLayout();
	topRow->setContentsMargins(0, 0, 0, 0);
	topRow->setSpacing(4);
	g_search = new QLineEdit(panel);
	g_search->setPlaceholderText("Search scenes");
	g_search->setClearButtonEnabled(true);
	QObject::connect(g_search, &QLineEdit::textChanged, panel,
			 [](const QString &) { applySearch(); });
	topRow->addWidget(g_search, 1);
	g_newBtn = new QToolButton(panel);
	g_newBtn->setAutoRaise(true);
	g_newBtn->setIcon(panel->style()->standardIcon(QStyle::SP_FileDialogNewFolder));
	g_newBtn->setToolTip("New folder");
	g_newBtn->setVisible(state().folderNewButton);
	QObject::connect(g_newBtn, &QToolButton::clicked, panel,
			 [panel]() { newFolderPrompt(panel); });
	topRow->addWidget(g_newBtn);
	v->addLayout(topRow);

	g_tree = new FolderTree(panel);
	g_tree->setHeaderHidden(true);
	g_tree->setDragDropMode(QAbstractItemView::InternalMove);
	g_tree->setSelectionMode(QAbstractItemView::SingleSelection);
	g_tree->setAnimated(true);
	g_tree->setIndentation(18);
	/* read as large and clear as the native Scenes panel: bigger font,
	   taller rows, bigger icons (metrics only; theme keeps its colors) */
	QFont treeFont = g_tree->font();
	treeFont.setPointSizeF(treeFont.pointSizeF() + 1.0);
	g_tree->setFont(treeFont);
	g_tree->setIconSize(QSize(18, 18));
	g_tree->setUniformRowHeights(true);
	g_tree->setStyleSheet("QTreeWidget::item { min-height: 30px; padding-left: 2px; }");
	g_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(g_tree, &QTreeWidget::customContextMenuRequested, g_tree,
			 [](const QPoint &pos) { showContextMenu(pos); });

	/* native panel keyboard parity, only while the tree has focus */
	QShortcut *renShort = new QShortcut(QKeySequence(Qt::Key_F2), g_tree);
	renShort->setContext(Qt::WidgetShortcut);
	QObject::connect(renShort, &QShortcut::activated, g_tree, []() {
		QTreeWidgetItem *it = g_tree ? g_tree->currentItem() : nullptr;
		if (isScene(it))
			renameScenePrompt(itemKey(it), it->text(0));
		else if (isFolder(it))
			renameFolderPrompt(g_tree, itemKey(it));
	});
	QShortcut *delShort = new QShortcut(QKeySequence(Qt::Key_Delete), g_tree);
	delShort->setContext(Qt::WidgetShortcut);
	QObject::connect(delShort, &QShortcut::activated, g_tree, []() {
		QTreeWidgetItem *it = g_tree ? g_tree->currentItem() : nullptr;
		if (isScene(it))
			removeScenePrompt(itemKey(it), it->text(0));
		else if (isFolder(it))
			deleteFolderPrompt(g_tree, itemKey(it));
	});
	v->addWidget(g_tree, 1);

	QObject::connect(g_tree, &QTreeWidget::itemClicked, g_tree,
			 [](QTreeWidgetItem *it, int) {
				 if (!isScene(it))
					 return;
				 obs_source_t *src = obs_get_source_by_uuid(
					 itemKey(it).toUtf8().constData());
				 if (!src)
					 return;
				 if (obs_frontend_preview_program_mode_active())
					 obs_frontend_set_current_preview_scene(src);
				 else
					 obs_frontend_set_current_scene(src);
				 obs_source_release(src);
			 });
	QObject::connect(g_tree, &QTreeWidget::itemExpanded, g_tree, [](QTreeWidgetItem *it) {
		if (g_applying || !isFolder(it))
			return;
		data().collapsed.remove(itemKey(it));
		stateSave();
	});
	QObject::connect(g_tree, &QTreeWidget::itemCollapsed, g_tree, [](QTreeWidgetItem *it) {
		if (g_applying || !isFolder(it))
			return;
		data().collapsed.insert(itemKey(it));
		stateSave();
	});

	if (!obs_frontend_add_dock_by_id("dockx_scene_folders", "Scene Folders", panel)) {
		obs_log(LOG_WARNING, "could not register the Scene Folders dock");
		delete panel;
		g_tree = nullptr;
		g_search = nullptr;
		g_newBtn = nullptr;
	}
}

void applySettings()
{
	if (g_newBtn)
		g_newBtn->setVisible(state().folderNewButton);
}

void showFirstRun()
{
	if (state().folderDockIntroduced || !g_tree)
		return;
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!m)
		return;
	/* the dock OBS wrapped our panel in; find by id, fall back to title */
	QDockWidget *dock = m->findChild<QDockWidget *>("dockx_scene_folders");
	if (!dock) {
		const QList<QDockWidget *> docks = m->findChildren<QDockWidget *>();
		for (QDockWidget *d : docks) {
			if (d->windowTitle() == QStringLiteral("Scene Folders")) {
				dock = d;
				break;
			}
		}
	}
	if (dock)
		dock->setVisible(true);
	state().folderDockIntroduced = true;
	stateSave();
}

void shutdown()
{
	g_shutdown = true;
	if (g_timer)
		g_timer->stop();
}

} // namespace folders
} // namespace dockx
