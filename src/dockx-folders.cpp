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
#include <QMainWindow>
#include <QMenu>
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

static void duplicateScene(const QString &uuid, const QString &name)
{
	obs_source_t *src = sceneByUuid(uuid);
	if (!src)
		return;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (scene) {
		const QString dupName = uniqueSceneName(name + " Copy");
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

		menu.addAction("Rename", [uuid, name]() { renameScenePrompt(uuid, name); });
		menu.addAction("Duplicate", [uuid, name]() { duplicateScene(uuid, name); });

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

		menu.addSeparator();
		menu.addAction("Filters", [uuid]() {
			if (obs_source_t *src = sceneByUuid(uuid)) {
				obs_frontend_open_source_filters(src);
				obs_source_release(src);
			}
		});
		QMenu *projMenu = menu.addMenu("Fullscreen Projector");
		const QList<QScreen *> screens = QGuiApplication::screens();
		for (int i = 0; i < screens.size(); i++) {
			const QRect g = screens[i]->geometry();
			projMenu->addAction(QString("Display %1 (%2x%3)")
						    .arg(i + 1)
						    .arg(g.width())
						    .arg(g.height()),
					    [i, name]() {
						    obs_frontend_open_projector(
							    "Scene", i, nullptr,
							    name.toUtf8().constData());
					    });
		}
		menu.addAction("Windowed Projector", [name]() {
			obs_frontend_open_projector("Scene", -1, nullptr,
						    name.toUtf8().constData());
		});
		menu.addAction("Screenshot", [uuid]() {
			if (obs_source_t *src = sceneByUuid(uuid)) {
				obs_frontend_take_source_screenshot(src);
				obs_source_release(src);
			}
		});
		menu.addSeparator();
		menu.addAction("Remove", [uuid, name]() { removeScenePrompt(uuid, name); });
	} else if (isFolder(it)) {
		const QString fname = itemKey(it);
		menu.addAction("New folder", []() { newFolderPrompt(g_tree); });
		menu.addAction("Rename", [fname]() { renameFolderPrompt(g_tree, fname); });
		menu.addAction("Delete", [fname]() { deleteFolderPrompt(g_tree, fname); });
		menu.addSeparator();
		menu.addAction("Collapse all", []() { setAllExpanded(false); });
		menu.addAction("Expand all", []() { setAllExpanded(true); });
	} else {
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

	g_search = new QLineEdit(panel);
	g_search->setPlaceholderText("Search scenes");
	g_search->setClearButtonEnabled(true);
	QObject::connect(g_search, &QLineEdit::textChanged, panel,
			 [](const QString &) { applySearch(); });
	v->addWidget(g_search);

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

	/* slim footer, like the native panel: one icon button; everything else
	   (rename/delete folder, all scene actions) lives in the right click menu */
	QHBoxLayout *row = new QHBoxLayout();
	row->setContentsMargins(0, 0, 0, 0);
	QToolButton *newBtn = new QToolButton(panel);
	newBtn->setAutoRaise(true);
	newBtn->setIcon(panel->style()->standardIcon(QStyle::SP_FileDialogNewFolder));
	newBtn->setToolTip("New folder");
	row->addWidget(newBtn);
	row->addStretch(1);
	v->addLayout(row);

	QObject::connect(newBtn, &QToolButton::clicked, panel,
			 [panel]() { newFolderPrompt(panel); });

	if (!obs_frontend_add_dock_by_id("dockx_scene_folders", "Scene Folders", panel)) {
		obs_log(LOG_WARNING, "could not register the Scene Folders dock");
		delete panel;
		g_tree = nullptr;
		g_search = nullptr;
	}
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
