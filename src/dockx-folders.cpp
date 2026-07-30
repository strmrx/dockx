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

#include <QBrush>
#include <QDropEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTimer>
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

static void rebuildNow()
{
	if (g_shutdown || !g_tree)
		return;
	g_applying = true;
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
		folderItems[fname] = fi;
	}

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
		}
	}

	for (auto it = folderItems.constBegin(); it != folderItems.constEnd(); ++it) {
		QTreeWidgetItem *fi = it.value();
		fi->setText(0, QString("%1  (%2)").arg(it.key()).arg(fi->childCount()));
		fi->setExpanded(!fd.collapsed.contains(it.key()));
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

static QTreeWidgetItem *selectedFolder(QWidget *msgParent)
{
	QTreeWidgetItem *it = g_tree ? g_tree->currentItem() : nullptr;
	if (!isFolder(it)) {
		QMessageBox::information(msgParent, "DockX", "Pick a folder first.");
		return nullptr;
	}
	return it;
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
	g_tree->setIndentation(14);
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

	QHBoxLayout *row = new QHBoxLayout();
	QPushButton *newBtn = new QPushButton("New folder", panel);
	QPushButton *renBtn = new QPushButton("Rename", panel);
	QPushButton *delBtn = new QPushButton("Delete", panel);
	row->addWidget(newBtn);
	row->addWidget(renBtn);
	row->addWidget(delBtn);
	row->addStretch(1);
	v->addLayout(row);

	QObject::connect(newBtn, &QPushButton::clicked, panel, [panel]() {
		bool ok = false;
		QString name = QInputDialog::getText(panel, "New folder",
						     "Folder name:", QLineEdit::Normal,
						     QString(), &ok);
		name = name.trimmed();
		if (!ok || name.isEmpty())
			return;
		FolderData &fd = data();
		if (fd.order.contains(name)) {
			QMessageBox::information(panel, "DockX",
						 "A folder with that name already exists.");
			return;
		}
		fd.order.append(name);
		stateSave();
		rebuildNow();
	});
	QObject::connect(renBtn, &QPushButton::clicked, panel, [panel]() {
		QTreeWidgetItem *it = selectedFolder(panel);
		if (!it)
			return;
		const QString oldName = itemKey(it);
		bool ok = false;
		QString name = QInputDialog::getText(panel, "Rename folder",
						     "New name:", QLineEdit::Normal, oldName,
						     &ok);
		name = name.trimmed();
		if (!ok || name.isEmpty() || name == oldName)
			return;
		FolderData &fd = data();
		if (fd.order.contains(name)) {
			QMessageBox::information(panel, "DockX",
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
	});
	QObject::connect(delBtn, &QPushButton::clicked, panel, [panel]() {
		QTreeWidgetItem *it = selectedFolder(panel);
		if (!it)
			return;
		const QString name = itemKey(it);
		auto answer = QMessageBox::question(
			panel, "Delete folder",
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
	});

	if (!obs_frontend_add_dock_by_id("dockx_scene_folders", "Scene Folders", panel)) {
		obs_log(LOG_WARNING, "could not register the Scene Folders dock");
		delete panel;
		g_tree = nullptr;
		g_search = nullptr;
	}
}

void shutdown()
{
	g_shutdown = true;
	if (g_timer)
		g_timer->stop();
}

} // namespace folders
} // namespace dockx
