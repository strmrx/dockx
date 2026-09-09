/*
DockX for OBS Studio (by StrmrX) -- the Scene Folders dock.
GPL v2, see plugin-main.cpp for the full notice.

A collapsible folder tree over the scene list, in its own dock. OBS's native
Scenes panel cannot be safely restructured, so we present our own view and
drive OBS through the frontend API. Folder assignments are keyed by scene
UUID (renames never lose a folder) and stored per scene collection.

Folders NEST: a folder is identified by its full path, segments joined by an
unprintable separator (so folder names may contain anything). Flat names from
older versions are simply single segment paths; no migration needed.

Two views share the dock: the tree, and a GRID (tiles like the native panel's
Grid Mode, but folder aware and color first: the whole tile carries the scene
color). The grid browses folders like a file explorer: click a folder tile to
enter it, the Up tile to leave. Searching in the grid flattens to matching
scenes from everywhere.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAction>
#include <QBrush>
#include <QColorDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QDockWidget>
#include <QDropEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>

namespace dockx {
namespace folders {

static QPointer<QTreeWidget> g_tree;
static QPointer<QListWidget> g_grid;
static QPointer<QLineEdit> g_search;
static QPointer<QToolButton> g_newBtn;
static QPointer<QToolButton> g_viewBtn;
static QPointer<QLabel> g_crumb;
static QPointer<QTimer> g_timer;
static bool g_applying = false;
static bool g_shutdown = false;
static QString g_gridPath; /* folder the grid is inside ("" = root); per session */

static QPointer<QTimer> g_thumbTimer; /* renders pending scene thumbnails, one per tick */
static QStringList g_thumbPending;    /* scene uuids waiting for a first render */

static const char *ROLE_TYPE_FOLDER = "f";
static const char *ROLE_TYPE_SCENE = "s";
static const char *ROLE_TYPE_UP = "u";
static const char *ROLE_TYPE_SOURCE = "x"; /* a source row under a scene */

/* ---- folder paths ---- */

static const QChar SEP(0x1F); /* unit separator: cannot appear in a typed name */

static QString pathName(const QString &path)
{
	const int i = path.lastIndexOf(SEP);
	return i < 0 ? path : path.mid(i + 1);
}

static QString pathParent(const QString &path)
{
	const int i = path.lastIndexOf(SEP);
	return i < 0 ? QString() : path.left(i);
}

static QString pathJoin(const QString &parent, const QString &name)
{
	return parent.isEmpty() ? name : parent + SEP + name;
}

static QString pathDisplay(const QString &path)
{
	QString out = path;
	return out.replace(SEP, QStringLiteral(" / "));
}

static bool pathInside(const QString &path, const QString &ancestor)
{
	return path == ancestor || path.startsWith(ancestor + SEP);
}

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

static bool isType(const QTreeWidgetItem *it, const char *t)
{
	return it && it->data(0, Qt::UserRole).toString() == QLatin1String(t);
}

static bool isFolder(const QTreeWidgetItem *it)
{
	return isType(it, ROLE_TYPE_FOLDER);
}

static bool isScene(const QTreeWidgetItem *it)
{
	return isType(it, ROLE_TYPE_SCENE);
}

static QString itemKey(const QTreeWidgetItem *it)
{
	return it->data(0, Qt::UserRole + 1).toString();
}

/* folder icon in the folder's color (or neutral); drawn, not themed, so any
   color works */
static QIcon folderGlyph(const QColor &c)
{
	QPixmap pm(32, 32);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	p.drawRoundedRect(QRectF(2, 3, 13, 9), 3, 3);  /* tab */
	p.drawRoundedRect(QRectF(2, 6, 28, 21), 4, 4); /* body */
	p.end();
	return QIcon(pm);
}

/* every scene row carries a dot in the leftmost slot; uncolored scenes get a
   faint neutral dot so rows stay aligned */
static const QColor NEUTRAL_DOT(160, 160, 160, 70);
static const QColor NEUTRAL_FOLDER(157, 157, 157);
static const QColor GRID_TILE_BG(52, 52, 52);

/* ---- source rows under scenes ---- */

static bool isSourceRow(const QTreeWidgetItem *it)
{
	return isType(it, ROLE_TYPE_SOURCE);
}

/* scenes expanded to show sources this session (deliberately not persisted:
   a fresh OBS start begins tidy, scenes collapsed) */
static QSet<QString> g_expandedScenes;

/* eye (open/closed) with a small amber padlock badge when locked */
static QIcon sourceGlyph(bool visible, bool locked)
{
	QPixmap pm(32, 32);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	p.setRenderHint(QPainter::Antialiasing);
	const QColor c = visible ? QColor(200, 200, 200) : QColor(150, 150, 150, 110);
	p.setPen(QPen(c, 2.4));
	p.setBrush(Qt::NoBrush);
	const QRectF eye(4, 9, 20, 14);
	p.drawArc(eye, 0, 180 * 16);
	p.drawArc(eye, 180 * 16, 180 * 16);
	p.setPen(Qt::NoPen);
	p.setBrush(c);
	p.drawEllipse(QPointF(14, 16), 3.4, 3.4);
	if (!visible) {
		p.setPen(QPen(c, 2.6));
		p.drawLine(QPointF(5, 26), QPointF(23, 6));
	}
	if (locked) {
		const QColor lc(255, 190, 80);
		p.setPen(Qt::NoPen);
		p.setBrush(lc);
		p.drawRoundedRect(QRectF(21, 19, 10, 9), 2, 2);
		p.setPen(QPen(lc, 2));
		p.setBrush(Qt::NoBrush);
		p.drawArc(QRectF(22.5, 13, 7, 9), 0, 180 * 16);
	}
	return QIcon(pm);
}

/* find a scene item by id anywhere in the scene, groups included */
struct FindItemCtx {
	long long id;
	obs_sceneitem_t *hit = nullptr;
};

static bool findItemEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	FindItemCtx *ctx = static_cast<FindItemCtx *>(param);
	if (obs_sceneitem_get_id(item) == ctx->id) {
		ctx->hit = item;
		return false;
	}
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, findItemEnum, param);
		if (ctx->hit)
			return false;
	}
	return true;
}

/* run fn(sceneitem) for the source row's live item; false if it is gone */
template<typename Fn> static bool withRowItem(const QTreeWidgetItem *row, Fn fn)
{
	if (!isSourceRow(row))
		return false;
	obs_source_t *sceneSrc = obs_get_source_by_uuid(row->data(0, Qt::UserRole + 1).toString().toUtf8().constData());
	if (!sceneSrc)
		return false;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	FindItemCtx ctx;
	ctx.id = row->data(0, Qt::UserRole + 2).toLongLong();
	if (scene)
		obs_scene_enum_items(scene, findItemEnum, &ctx);
	if (ctx.hit)
		fn(ctx.hit);
	obs_source_release(sceneSrc);
	return ctx.hit != nullptr;
}

static bool collectItemsEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
	return true;
}

static void addSourceRow(QTreeWidgetItem *parent, const QString &sceneUuid, obs_sceneitem_t *item)
{
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (!src)
		return;
	QTreeWidgetItem *row = new QTreeWidgetItem(parent);
	row->setText(0, QString::fromUtf8(obs_source_get_name(src)));
	row->setData(0, Qt::UserRole, ROLE_TYPE_SOURCE);
	row->setData(0, Qt::UserRole + 1, sceneUuid);
	row->setData(0, Qt::UserRole + 2, (qlonglong)obs_sceneitem_get_id(item));
	/* selectable for the context menu; never draggable, never a drop target */
	row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
	const bool vis = obs_sceneitem_visible(item);
	row->setIcon(0, sourceGlyph(vis, obs_sceneitem_locked(item)));
	row->setToolTip(0, "Click the eye to show or hide this source.");
	if (!vis)
		row->setForeground(0, QBrush(QColor(150, 150, 150, 140)));
	if (obs_sceneitem_is_group(item)) {
		std::vector<obs_sceneitem_t *> kids;
		obs_sceneitem_group_enum_items(item, collectItemsEnum, &kids);
		/* enum is bottom first; show top first like the Sources panel */
		for (auto k = kids.rbegin(); k != kids.rend(); ++k)
			addSourceRow(row, sceneUuid, *k);
	}
}

static void addSourceRows(QTreeWidgetItem *sceneItem, const QString &sceneUuid)
{
	obs_source_t *sceneSrc = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!sceneSrc)
		return;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	if (scene) {
		std::vector<obs_sceneitem_t *> items;
		obs_scene_enum_items(scene, collectItemsEnum, &items);
		for (auto it = items.rbegin(); it != items.rend(); ++it)
			addSourceRow(sceneItem, sceneUuid, *it);
	}
	obs_source_release(sceneSrc);
}

/* ---- shared scene data ---- */

struct SceneRow {
	QString uuid;
	QString name;
};

static std::vector<SceneRow> sceneRows()
{
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
	return scenes;
}

static QString currentSceneUuid()
{
	obs_source_t *cur = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
								       : obs_frontend_get_current_scene();
	QString out;
	if (cur) {
		const char *u = obs_source_get_uuid(cur);
		if (u)
			out = QString::fromUtf8(u);
		obs_source_release(cur);
	}
	return out;
}

/* ---- search (tree) ---- */

/* source rows: visible when they (or a group child) match; a matching scene
   or group shows everything under it */
static bool applySearchSource(QTreeWidgetItem *it, const QString &q, bool showAll)
{
	const bool nameHit = q.isEmpty() || it->text(0).contains(q, Qt::CaseInsensitive);
	bool anyKid = false;
	for (int j = 0; j < it->childCount(); j++)
		if (applySearchSource(it->child(j), q, showAll || nameHit))
			anyKid = true;
	const bool show = showAll || nameHit || anyKid;
	it->setHidden(!show);
	if (!q.isEmpty() && anyKid)
		it->setExpanded(true);
	return show;
}

static bool applySearchItem(QTreeWidgetItem *it, const QString &q)
{
	if (isScene(it)) {
		const bool nameHit = q.isEmpty() || it->text(0).contains(q, Qt::CaseInsensitive);
		bool anyKid = false;
		for (int j = 0; j < it->childCount(); j++)
			if (applySearchSource(it->child(j), q, nameHit))
				anyKid = true;
		const bool show = q.isEmpty() || nameHit || anyKid;
		it->setHidden(!show);
		/* a source hit peeks into its scene so you see WHERE it lives */
		if (!q.isEmpty() && anyKid && !nameHit)
			it->setExpanded(true);
		return show;
	}
	bool anyVisible = false;
	for (int j = 0; j < it->childCount(); j++)
		if (applySearchItem(it->child(j), q))
			anyVisible = true;
	const bool nameHit = !q.isEmpty() && pathName(itemKey(it)).contains(q, Qt::CaseInsensitive);
	it->setHidden(!q.isEmpty() && !anyVisible && !nameHit);
	if (!q.isEmpty() && anyVisible)
		it->setExpanded(true); /* searching peeks into folders */
	return !it->isHidden();
}

static void rebuildGrid();

static void applySearch()
{
	if (!g_tree)
		return;
	const QString q = g_search ? g_search->text().trimmed() : QString();
	for (int i = 0; i < g_tree->topLevelItemCount(); i++)
		applySearchItem(g_tree->topLevelItem(i), q);
	if (state().folderGridMode)
		rebuildGrid(); /* grid search = flattened matches */
}

static QTreeWidgetItem *findByKeyIn(QTreeWidgetItem *it, const QString &key)
{
	if (itemKey(it) == key)
		return it;
	for (int j = 0; j < it->childCount(); j++)
		if (QTreeWidgetItem *hit = findByKeyIn(it->child(j), key))
			return hit;
	return nullptr;
}

static QTreeWidgetItem *findByKey(const QString &key)
{
	for (int i = 0; i < g_tree->topLevelItemCount(); i++)
		if (QTreeWidgetItem *hit = findByKeyIn(g_tree->topLevelItem(i), key))
			return hit;
	return nullptr;
}

/* ---- tree rebuild ---- */

static QTreeWidgetItem *ensureFolderItem(const QString &path, QHash<QString, QTreeWidgetItem *> &items, FolderData &fd)
{
	if (path.isEmpty())
		return nullptr;
	auto found = items.find(path);
	if (found != items.end())
		return found.value();
	QTreeWidgetItem *parent = ensureFolderItem(pathParent(path), items, fd);
	QTreeWidgetItem *fi = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(g_tree);
	fi->setData(0, Qt::UserRole, ROLE_TYPE_FOLDER);
	fi->setData(0, Qt::UserRole + 1, path);
	fi->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
	const QString fhex = fd.colors.value(path);
	fi->setData(0, Qt::UserRole + 2, fhex);
	fi->setIcon(0, folderGlyph(fhex.isEmpty() ? NEUTRAL_FOLDER : QColor(fhex)));
	if (!fhex.isEmpty())
		fi->setForeground(0, QBrush(QColor(fhex)));
	QFont ff = fi->font(0);
	ff.setBold(true);
	fi->setFont(0, ff);
	items[path] = fi;
	return fi;
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
	const std::vector<SceneRow> scenes = sceneRows();

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
	for (const QString &fpath : fd.assign)
		if (!fd.order.contains(fpath))
			fd.order.append(fpath);

	const QString curUuid = currentSceneUuid();

	QHash<QString, QTreeWidgetItem *> folderItems;
	for (const QString &fpath : fd.order)
		ensureFolderItem(fpath, folderItems, fd);

	QTreeWidgetItem *curItem = nullptr;
	for (const SceneRow &s : scenes) {
		QTreeWidgetItem *parent = folderItems.value(fd.assign.value(s.uuid), nullptr);
		QTreeWidgetItem *si = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(g_tree);
		si->setText(0, s.name);
		si->setData(0, Qt::UserRole, ROLE_TYPE_SCENE);
		si->setData(0, Qt::UserRole + 1, s.uuid);
		si->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
		const QString hex = state().sceneColors ? state().colors.value(s.name) : QString();
		if (!hex.isEmpty()) {
			si->setForeground(0, QBrush(QColor(hex)));
			si->setIcon(0, colorDot(QColor(hex)));
		} else {
			si->setIcon(0, colorDot(NEUTRAL_DOT));
		}
		if (s.uuid == curUuid) {
			QFont f = si->font(0);
			f.setBold(true);
			si->setFont(0, f);
			si->setSelected(true);
			curItem = si;
		}
		if (state().folderSources) {
			addSourceRows(si, s.uuid);
			si->setExpanded(g_expandedScenes.contains(s.uuid));
		}
	}

	for (auto it = folderItems.constBegin(); it != folderItems.constEnd(); ++it) {
		QTreeWidgetItem *fi = it.value();
		int sceneCount = 0;
		for (int j = 0; j < fi->childCount(); j++)
			if (isScene(fi->child(j)))
				sceneCount++;
		fi->setText(0, QString("%1  (%2)").arg(pathName(it.key())).arg(sceneCount));
		fi->setExpanded(!fd.collapsed.contains(it.key()));
	}

	/* put the list back where it was before the rebuild */
	if (!topKey.isEmpty()) {
		if (QTreeWidgetItem *top = findByKey(topKey))
			g_tree->scrollToItem(top, QAbstractItemView::PositionAtTop);
	}

	/* always show where you are: peek into folders holding the live scene,
	   but only scroll if it is actually off screen (native panel behavior; a
	   collapsed folder stays collapsed next rebuild) */
	if (curItem) {
		for (QTreeWidgetItem *p = curItem->parent(); p; p = p->parent())
			p->setExpanded(true);
		const QRect r = g_tree->visualItemRect(curItem);
		const int viewH = g_tree->viewport()->height();
		if (r.height() == 0 || r.top() < 0 || r.bottom() > viewH)
			g_tree->scrollToItem(curItem);
	}

	g_applying = false;
	applySearch();
	rebuildGrid();
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

/* ---- persist the tree after a drag ---- */

static void walkPersist(QTreeWidgetItem *it, const QString &parentPath, FolderData &fd)
{
	if (isScene(it)) {
		if (!parentPath.isEmpty())
			fd.assign[itemKey(it)] = parentPath;
		return;
	}
	if (!isFolder(it))
		return;
	const QString path = pathJoin(parentPath, pathName(itemKey(it)));
	fd.order << path;
	const QString hex = it->data(0, Qt::UserRole + 2).toString();
	if (!hex.isEmpty())
		fd.colors[path] = hex;
	if (!it->isExpanded())
		fd.collapsed.insert(path);
	it->setData(0, Qt::UserRole + 1, path); /* the move may have changed it */
	for (int j = 0; j < it->childCount(); j++)
		walkPersist(it->child(j), path, fd);
}

static void persistFromTree()
{
	if (!g_tree || g_applying)
		return;
	/* nesting off: any folder dropped inside a folder pops back to the top */
	if (!state().folderNesting) {
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
	}
	FolderData &fd = data();
	fd.assign.clear();
	fd.order.clear();
	fd.colors.clear();
	fd.collapsed.clear();
	for (int i = 0; i < g_tree->topLevelItemCount(); i++)
		walkPersist(g_tree->topLevelItem(i), QString(), fd);
	stateSave();
	rebuildSoon();
}

/* ---- scene ordering: drag to reorder + A to Z ---- */

/* push a display order of scene names into the NATIVE scenes list, silently.
   OBS saves list order from that widget and obs_frontend_get_scenes follows
   it, so the order sticks without any DockX-side persistence. */
static void reorderNativeTo(const QStringList &names)
{
	QListWidget *list = panels::nativeSceneList();
	if (!list)
		return;
	QListWidgetItem *cur = list->currentItem();
	const QSignalBlocker block(list);
	int insertPos = 0;
	for (const QString &name : names) {
		int row = -1;
		for (int i = insertPos; i < list->count(); i++) {
			if (list->item(i)->text() == name) {
				row = i;
				break;
			}
		}
		if (row < 0)
			continue; /* unknown name: touch nothing else */
		if (row != insertPos) {
			QListWidgetItem *it = list->takeItem(row);
			list->insertItem(insertPos, it);
		}
		insertPos++;
	}
	if (cur)
		list->setCurrentItem(cur);
}

/* walk the tree collecting scene names in display order; a container matching
   sortPath (or every container when sortAll) emits its direct scenes A to Z */
static void collectOrdered(QTreeWidgetItem *folder, QStringList &out, bool sortAll, const QString &sortPath)
{
	if (!g_tree)
		return;
	const int n = folder ? folder->childCount() : g_tree->topLevelItemCount();
	auto childAt = [folder](int i) {
		return folder ? folder->child(i) : g_tree->topLevelItem(i);
	};
	const bool sortHere = sortAll || (folder && !sortPath.isEmpty() && itemKey(folder) == sortPath);
	QStringList sorted;
	if (sortHere) {
		for (int i = 0; i < n; i++) {
			QTreeWidgetItem *c = childAt(i);
			if (isScene(c))
				sorted << c->text(0);
		}
		std::sort(sorted.begin(), sorted.end(),
			  [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
	}
	int next = 0;
	for (int i = 0; i < n; i++) {
		QTreeWidgetItem *c = childAt(i);
		if (isScene(c))
			out << (sortHere ? sorted[next++] : c->text(0));
		else if (isFolder(c))
			collectOrdered(c, out, sortAll, sortPath);
	}
}

/* after a drag: the native list follows the tree's scene order */
static void applySceneOrderFromTree()
{
	QStringList want;
	collectOrdered(nullptr, want, false, QString());
	reorderNativeTo(want);
}

/* sortPath = one folder's direct scenes; all = every container incl. root */
static void sortScenesAtoZ(bool all, const QString &folderPath)
{
	QStringList want;
	collectOrdered(nullptr, want, all, folderPath);
	reorderNativeTo(want);
	rebuildSoon();
}

class FolderTree : public QTreeWidget {
public:
	using QTreeWidget::QTreeWidget;

protected:
	void dropEvent(QDropEvent *e) override
	{
		/* a folder must never land inside itself */
		QTreeWidgetItem *src = currentItem();
		QTreeWidgetItem *dst = itemAt(e->position().toPoint());
		if (isFolder(src) && dst) {
			for (QTreeWidgetItem *p = dst; p; p = p->parent()) {
				if (p == src) {
					e->ignore();
					return;
				}
			}
		}
		QTreeWidget::dropEvent(e);
		applySceneOrderFromTree();
		persistFromTree();
	}

	/* arrows only in the branch column; none of the dotted connector lines.
	   The gutter is painted opaque first so no theme decoration survives. */
	void drawBranches(QPainter *painter, const QRect &rect, const QModelIndex &index) const override
	{
		painter->fillRect(rect, viewport()->palette().color(viewport()->backgroundRole()));
		if (!model()->hasChildren(index))
			return;
		QStyleOption opt;
		opt.initFrom(this);
		const int s = 14;
		QRect r(rect.right() - indentation() + (indentation() - s) / 2, rect.center().y() - s / 2, s, s);
		opt.rect = r;
		style()->drawPrimitive(isExpanded(index) ? QStyle::PE_IndicatorArrowDown
							 : QStyle::PE_IndicatorArrowRight,
				       &opt, painter, this);
	}
};

/* ---- folder management ---- */

static void newFolderPrompt(QWidget *parent, const QString &assignSceneUuid = QString(),
			    const QString &parentPath = QString())
{
	bool ok = false;
	QString name =
		QInputDialog::getText(parent, "New folder", "Folder name:", QLineEdit::Normal, QString(), &ok).trimmed();
	name.remove(SEP);
	if (!ok || name.isEmpty())
		return;
	FolderData &fd = data();
	const QString path = pathJoin(parentPath, name);
	if (fd.order.contains(path)) {
		QMessageBox::information(parent, "DockX", "A folder with that name already exists here.");
		return;
	}
	fd.order.append(path);
	if (!assignSceneUuid.isEmpty())
		fd.assign[assignSceneUuid] = path;
	stateSave();
	rebuildNow();
}

static void renameFolderPrompt(QWidget *parent, const QString &oldPath)
{
	bool ok = false;
	QString name =
		QInputDialog::getText(parent, "Rename folder", "New name:", QLineEdit::Normal, pathName(oldPath), &ok)
			.trimmed();
	name.remove(SEP);
	if (!ok || name.isEmpty() || name == pathName(oldPath))
		return;
	FolderData &fd = data();
	const QString newPath = pathJoin(pathParent(oldPath), name);
	if (fd.order.contains(newPath)) {
		QMessageBox::information(parent, "DockX", "A folder with that name already exists here.");
		return;
	}
	auto remap = [&](const QString &p) {
		return p == oldPath ? newPath : p.startsWith(oldPath + SEP) ? newPath + p.mid(oldPath.length()) : p;
	};
	for (int i = 0; i < fd.order.size(); i++)
		fd.order[i] = remap(fd.order[i]);
	for (auto a = fd.assign.begin(); a != fd.assign.end(); ++a)
		a.value() = remap(a.value());
	QSet<QString> newCollapsed;
	for (const QString &p : fd.collapsed)
		newCollapsed.insert(remap(p));
	fd.collapsed = newCollapsed;
	QHash<QString, QString> newColors;
	for (auto c = fd.colors.constBegin(); c != fd.colors.constEnd(); ++c)
		newColors[remap(c.key())] = c.value();
	fd.colors = newColors;
	stateSave();
	rebuildNow();
}

static void setFolderColor(const QString &path, const QString &hex)
{
	FolderData &fd = data();
	if (hex.isEmpty())
		fd.colors.remove(path);
	else
		fd.colors[path] = hex;
	stateSave();
	rebuildNow();
}

static void deleteFolderPrompt(QWidget *parent, const QString &path)
{
	auto answer = QMessageBox::question(parent, "Delete folder",
					    QString("Delete \"%1\"? Subfolders are deleted too and the scenes "
						    "inside go back to the main list. No scene is deleted.")
						    .arg(pathName(path)));
	if (answer != QMessageBox::Yes)
		return;
	FolderData &fd = data();
	for (int i = fd.order.size() - 1; i >= 0; i--)
		if (pathInside(fd.order[i], path))
			fd.order.removeAt(i);
	for (auto it = fd.collapsed.begin(); it != fd.collapsed.end();) {
		if (pathInside(*it, path))
			it = fd.collapsed.erase(it);
		else
			++it;
	}
	for (auto it = fd.colors.begin(); it != fd.colors.end();) {
		if (pathInside(it.key(), path))
			it = fd.colors.erase(it);
		else
			++it;
	}
	for (auto a = fd.assign.begin(); a != fd.assign.end();) {
		if (pathInside(a.value(), path))
			a = fd.assign.erase(a);
		else
			++a;
	}
	if (pathInside(g_gridPath, path))
		g_gridPath = pathParent(path);
	stateSave();
	rebuildNow();
}

static void setAllExpandedItem(QTreeWidgetItem *it, bool on)
{
	if (!isFolder(it))
		return;
	it->setExpanded(on);
	for (int j = 0; j < it->childCount(); j++)
		setAllExpandedItem(it->child(j), on);
}

static void setAllExpanded(bool on)
{
	if (!g_tree)
		return;
	for (int i = 0; i < g_tree->topLevelItemCount(); i++)
		setAllExpandedItem(g_tree->topLevelItem(i), on);
}

/* ---- scene actions ---- */

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
	QString newName =
		QInputDialog::getText(g_tree, "Rename scene", "New name:", QLineEdit::Normal, name, &ok).trimmed();
	if (!ok || newName.isEmpty() || newName == name)
		return;
	if (obs_source_t *clash = obs_get_source_by_name(newName.toUtf8().constData())) {
		obs_source_release(clash);
		QMessageBox::information(g_tree, "DockX", "A scene or source with that name already exists.");
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
	QString name = QInputDialog::getText(g_tree, title, "Scene name:", QLineEdit::Normal, suggested, &ok).trimmed();
	if (!ok || name.isEmpty())
		return QString();
	if (obs_source_t *clash = obs_get_source_by_name(name.toUtf8().constData())) {
		obs_source_release(clash);
		QMessageBox::information(g_tree, "DockX", "A scene or source with that name already exists.");
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
	const QString dupName = promptSceneName("Duplicate scene", uniqueSceneName(name + " Copy"));
	if (dupName.isEmpty())
		return;
	obs_source_t *src = sceneByUuid(uuid);
	if (!src)
		return;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (scene) {
		obs_scene_t *dup = obs_scene_duplicate(scene, dupName.toUtf8().constData(), OBS_SCENE_DUP_REFS);
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
	const int dst = mode == 0 ? row - 1 : mode == 1 ? row + 1 : mode == 2 ? 0 : list->count() - 1;
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
				  QString("Remove \"%1\" from OBS? This deletes the scene.").arg(name)) !=
	    QMessageBox::Yes)
		return;
	if (obs_source_t *src = sceneByUuid(uuid)) {
		obs_source_remove(src);
		obs_source_release(src);
	}
}

/* ---- context menus (shared by tree and grid) ---- */

static void copyToCollectionPrompt(const QString &uuid, const QString &name, const QString &target)
{
	QWidget *parent = g_tree ? static_cast<QWidget *>(g_tree) : nullptr;
	const auto answer =
		QMessageBox::question(parent, "Copy scene to collection",
				      QString("Copy \"%1\" into the \"%2\" scene collection?\n\nIt is ADDED to "
					      "that collection (nothing there is overwritten), and a backup of "
					      "the collection is saved first. You'll see the scene when you "
					      "switch to \"%2\".")
					      .arg(name, target));
	if (answer != QMessageBox::Yes)
		return;
	const collections::CopyReport r = collections::copySceneToCollection(uuid, target);
	if (!r.ok) {
		QMessageBox::warning(parent, "DockX", r.error);
		return;
	}
	QString msg = QString("Copied \"%1\" into \"%2\".").arg(r.finalSceneName, target);
	if (r.finalSceneName != name)
		msg += QString("\n\nRenamed to \"%1\" (that name was already used there).").arg(r.finalSceneName);
	msg += QString("\n\n%1 source(s) copied").arg(r.sourcesCopied);
	if (r.sourcesSkipped > 0)
		msg += QString(", %1 already existed and were left as is").arg(r.sourcesSkipped);
	msg += ".";
	QMessageBox::information(parent, "DockX", msg);
}

static void buildSceneMenu(QMenu &menu, const QString &uuid, const QString &name)
{
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
		curOverride = QString::fromUtf8(obs_data_get_string(priv, "transition"));
		curDur = (int)obs_data_get_int(priv, "transition_duration");
		obs_data_release(priv);
		obs_source_release(src);
	}

	menu.addAction("Add Scene...", [uuid]() { addScenePrompt(data().assign.value(uuid)); });
	menu.addAction("Duplicate...", [uuid, name]() { duplicateScenePrompt(uuid, name); });
	QMenu *copyToMenu = menu.addMenu("Copy to Collection");
	const QStringList others = collections::otherCollections();
	if (others.isEmpty()) {
		QAction *none = copyToMenu->addAction("No other collections");
		none->setEnabled(false);
	} else {
		for (const QString &target : others)
			copyToMenu->addAction(target,
					      [uuid, name, target]() { copyToCollectionPrompt(uuid, name, target); });
	}
	QAction *copyF = menu.addAction("Copy Filters", [uuid]() { g_copyFiltersUuid = uuid; });
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
	QAction *renA = menu.addAction("Rename...", [uuid, name]() { renameScenePrompt(uuid, name); });
	renA->setShortcut(QKeySequence(Qt::Key_F2));
	renA->setShortcutContext(Qt::WidgetShortcut);
	renA->setShortcutVisibleInContextMenu(true);
	QAction *remA = menu.addAction("Remove", [uuid, name]() { removeScenePrompt(uuid, name); });
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
	for (const QString &fpath : fd.order) {
		QAction *a = moveMenu->addAction(pathDisplay(fpath), [uuid, fpath]() {
			data().assign[uuid] = fpath;
			stateSave();
			rebuildNow();
		});
		a->setCheckable(true);
		a->setChecked(fpath == curFolder);
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
	moveMenu->addAction("New folder...", [uuid]() { newFolderPrompt(g_tree, uuid); });

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
	colorMenu->addAction("No color", [name]() { setSceneColor(name, QString()); });
	menu.addAction("Add Video Dock", [name]() { sourcedocks::addDock(sourcedocks::KIND_SOURCE, name); });

	menu.addSeparator();
	QMenu *projMenu = menu.addMenu("Open Scene Projector");
	QMenu *fsMenu = projMenu->addMenu("Fullscreen");
	const QList<QScreen *> screens = QGuiApplication::screens();
	for (int i = 0; i < screens.size(); i++) {
		const QRect g = screens[i]->geometry();
		fsMenu->addAction(QString("Display %1 (%2x%3)").arg(i + 1).arg(g.width()).arg(g.height()), [i, name]() {
			obs_frontend_open_projector("Scene", i, nullptr, name.toUtf8().constData());
		});
	}
	projMenu->addAction("Windowed",
			    [name]() { obs_frontend_open_projector("Scene", -1, nullptr, name.toUtf8().constData()); });
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
	QAction *noneT = trMenu->addAction("None", [uuid]() { setTransitionOverride(uuid, QString()); });
	noneT->setCheckable(true);
	noneT->setChecked(curOverride.isEmpty());
	obs_frontend_source_list tl = {};
	obs_frontend_get_transitions(&tl);
	for (size_t i = 0; i < tl.sources.num; i++) {
		const char *tn = obs_source_get_name(tl.sources.array[i]);
		if (!tn)
			continue;
		const QString tname = QString::fromUtf8(tn);
		QAction *a = trMenu->addAction(tname, [uuid, tname]() { setTransitionOverride(uuid, tname); });
		a->setCheckable(true);
		a->setChecked(tname == curOverride);
	}
	obs_frontend_source_list_free(&tl);
	trMenu->addSeparator();
	trMenu->addAction(QString("Duration (%1 ms)...").arg(curDur), [uuid, curDur]() {
		bool ok = false;
		const int ms = QInputDialog::getInt(g_tree, "Transition duration", "Milliseconds:", curDur, 50, 20000,
						    50, &ok);
		if (!ok)
			return;
		obs_source_t *s = sceneByUuid(uuid);
		if (!s)
			return;
		obs_data_t *priv = obs_source_get_private_settings(s);
		obs_data_set_int(priv, "transition_duration", ms);
		obs_data_release(priv);
		obs_source_release(s);
	});
	QAction *mv = menu.addAction("Show in Multiview");
	mv->setCheckable(true);
	mv->setChecked(inMultiview);
	QObject::connect(mv, &QAction::triggered, &menu, [uuid](bool on) {
		obs_source_t *s = sceneByUuid(uuid);
		if (!s)
			return;
		obs_data_t *priv = obs_source_get_private_settings(s);
		obs_data_set_bool(priv, "show_in_multiview", on);
		obs_data_release(priv);
		obs_source_release(s);
	});
	menu.addSeparator();
	menu.addAction("Lock all sources", [uuid]() {
		loadouts::lockScene(uuid, true);
		rebuildSoon();
	});
	menu.addAction("Unlock all sources", [uuid]() {
		loadouts::lockScene(uuid, false);
		rebuildSoon();
	});
}

/* right click on a source row: the everyday per source controls */
static void buildSourceMenu(QMenu &menu, QTreeWidgetItem *row)
{
	bool visible = true, locked = false;
	withRowItem(row, [&visible, &locked](obs_sceneitem_t *item) {
		visible = obs_sceneitem_visible(item);
		locked = obs_sceneitem_locked(item);
	});
	/* the row outlives the menu closures only until the next rebuild, so
	   capture the lookup keys, never the row pointer */
	const QString sceneUuid = row->data(0, Qt::UserRole + 1).toString();
	const qlonglong itemId = row->data(0, Qt::UserRole + 2).toLongLong();
	auto withItem = [sceneUuid, itemId](std::function<void(obs_sceneitem_t *)> fn) {
		obs_source_t *sceneSrc = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
		if (!sceneSrc)
			return;
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		FindItemCtx ctx;
		ctx.id = itemId;
		if (scene)
			obs_scene_enum_items(scene, findItemEnum, &ctx);
		if (ctx.hit)
			fn(ctx.hit);
		obs_source_release(sceneSrc);
	};

	menu.addAction(visible ? "Hide" : "Show", [withItem, visible]() {
		withItem([visible](obs_sceneitem_t *item) { obs_sceneitem_set_visible(item, !visible); });
		rebuildSoon();
	});
	menu.addAction(locked ? "Unlock" : "Lock", [withItem, locked]() {
		withItem([locked](obs_sceneitem_t *item) { obs_sceneitem_set_locked(item, !locked); });
		rebuildSoon();
	});
	menu.addSeparator();
	menu.addAction("Filters", [withItem]() {
		withItem([](obs_sceneitem_t *item) {
			obs_frontend_open_source_filters(obs_sceneitem_get_source(item));
		});
	});
	menu.addAction("Properties", [withItem]() {
		withItem([](obs_sceneitem_t *item) {
			obs_frontend_open_source_properties(obs_sceneitem_get_source(item));
		});
	});
	menu.addAction("Rename...", [withItem]() {
		withItem([](obs_sceneitem_t *item) {
			obs_source_t *src = obs_sceneitem_get_source(item);
			if (!src)
				return;
			bool ok = false;
			const QString name = QInputDialog::getText(g_tree, "Rename source",
								   "New name:", QLineEdit::Normal,
								   QString::fromUtf8(obs_source_get_name(src)), &ok);
			if (ok && !name.trimmed().isEmpty())
				obs_source_set_name(src, name.trimmed().toUtf8().constData());
		});
		rebuildSoon();
	});
}

static void buildFolderMenu(QMenu &menu, const QString &path)
{
	menu.addAction("Add Scene...", [path]() { addScenePrompt(path); });
	menu.addAction("New folder", []() { newFolderPrompt(g_tree); });
	if (state().folderNesting)
		menu.addAction("New subfolder", [path]() { newFolderPrompt(g_tree, QString(), path); });
	menu.addAction("Sort scenes A to Z", [path]() { sortScenesAtoZ(false, path); });
	menu.addAction("Rename", [path]() { renameFolderPrompt(g_tree, path); });
	menu.addAction("Delete", [path]() { deleteFolderPrompt(g_tree, path); });
	QMenu *fcMenu = menu.addMenu("Set Color");
	for (int i = 0; i < 8; i++) {
		const QString hex = QString::fromUtf8(PRESET_COLORS[i]);
		fcMenu->addAction(folderGlyph(QColor(hex)), PRESET_COLOR_NAMES[i],
				  [path, hex]() { setFolderColor(path, hex); });
	}
	fcMenu->addAction("Custom...", [path]() {
		QColor c = QColorDialog::getColor(Qt::white, g_tree, "Pick a color");
		if (c.isValid())
			setFolderColor(path, c.name());
	});
	fcMenu->addAction("No color", [path]() { setFolderColor(path, QString()); });
}

static void showContextMenu(const QPoint &pos)
{
	if (!g_tree)
		return;
	QTreeWidgetItem *it = g_tree->itemAt(pos);
	QMenu menu(g_tree);

	if (isSourceRow(it)) {
		buildSourceMenu(menu, it);
	} else if (isScene(it)) {
		buildSceneMenu(menu, itemKey(it), it->text(0));
	} else if (isFolder(it)) {
		buildFolderMenu(menu, itemKey(it));
		menu.addSeparator();
		menu.addAction("Collapse all", []() { setAllExpanded(false); });
		menu.addAction("Expand all", []() { setAllExpanded(true); });
	} else {
		menu.addAction("Add Scene...", []() { addScenePrompt(QString()); });
		menu.addAction("New folder", []() { newFolderPrompt(g_tree); });
		menu.addAction("Sort all scenes A to Z", []() { sortScenesAtoZ(true, QString()); });
		menu.addSeparator();
		menu.addAction("Lock every source (all scenes)", []() {
			loadouts::lockAll(true);
			rebuildSoon();
		});
		menu.addAction("Unlock every source (all scenes)", []() {
			loadouts::lockAll(false);
			rebuildSoon();
		});
		menu.addSeparator();
		menu.addAction("Collapse all", []() { setAllExpanded(false); });
		menu.addAction("Expand all", []() { setAllExpanded(true); });
	}

	menu.exec(g_tree->viewport()->mapToGlobal(pos));
}

/* ---- the grid view ---- */

static void updateCrumb()
{
	if (!g_crumb)
		return;
	const bool show = state().folderGridMode && !g_gridPath.isEmpty();
	g_crumb->setVisible(show);
	if (show)
		g_crumb->setText(QString("Folder: %1").arg(pathDisplay(g_gridPath)));
}

/* ---- scene thumbnails in the grid ---- */

static QIcon thumbPlaceholder(const QColor &bg)
{
	QPixmap pm(thumbs::size());
	pm.fill(bg.darker(115));
	return QIcon(pm);
}

/* drop a freshly rendered thumbnail onto its scene tile, if it is still shown */
static void setGridThumb(const QString &uuid, const QPixmap &pm)
{
	if (!g_grid || pm.isNull())
		return;
	for (int i = 0; i < g_grid->count(); i++) {
		QListWidgetItem *it = g_grid->item(i);
		if (it->data(Qt::UserRole).toString() == QLatin1String(ROLE_TYPE_SCENE) &&
		    it->data(Qt::UserRole + 1).toString() == uuid) {
			it->setIcon(QIcon(pm));
			return;
		}
	}
}

/* render one queued thumbnail per tick so a big collection fills in smoothly
   instead of hitching the whole grid at once */
static void serviceThumbs()
{
	if (g_shutdown || !g_grid || !state().sceneThumbs) {
		g_thumbPending.clear();
		if (g_thumbTimer)
			g_thumbTimer->stop();
		return;
	}
	while (!g_thumbPending.isEmpty()) {
		const QString uuid = g_thumbPending.takeFirst();
		if (!thumbs::cached(uuid).isNull())
			continue; /* filled in already */
		const QPixmap pm = thumbs::render(uuid);
		if (!pm.isNull())
			setGridThumb(uuid, pm);
		return; /* just one per tick */
	}
	if (g_thumbTimer)
		g_thumbTimer->stop();
}

static void startThumbTimer()
{
	if (!g_thumbTimer) {
		g_thumbTimer = new QTimer(g_grid);
		g_thumbTimer->setInterval(120);
		QObject::connect(g_thumbTimer, &QTimer::timeout, []() { serviceThumbs(); });
	}
	if (!g_thumbPending.isEmpty() && !g_thumbTimer->isActive())
		g_thumbTimer->start();
}

static void rebuildGrid()
{
	if (g_shutdown || !g_grid || !state().folderGridMode)
		return;
	const bool thumbsOn = state().sceneThumbs;
	g_grid->setIconSize(thumbsOn ? thumbs::size() : QSize(20, 20));
	g_grid->setGridSize(thumbsOn ? QSize(thumbs::size().width() + 16, thumbs::size().height() + 34)
				     : QSize(112, 66));
	g_thumbPending.clear();
	const bool wasApplying = g_applying;
	g_applying = true;
	g_grid->clear();

	FolderData &fd = data();
	/* the folder we are in may be gone (collection switch etc.) */
	if (!g_gridPath.isEmpty() && !fd.order.contains(g_gridPath))
		g_gridPath.clear();
	updateCrumb();

	const QString q = g_search ? g_search->text().trimmed() : QString();
	const QString curUuid = currentSceneUuid();

	auto contrastText = [](const QColor &bg) {
		return bg.lightness() > 140 ? QColor(Qt::black) : QColor(Qt::white);
	};
	auto addTile = [&](const char *type, const QString &key, const QString &text, const QColor &bg,
			   const QIcon &icon) {
		QListWidgetItem *it = new QListWidgetItem(text, g_grid);
		it->setData(Qt::UserRole, QLatin1String(type));
		it->setData(Qt::UserRole + 1, key);
		it->setBackground(QBrush(bg));
		it->setForeground(QBrush(contrastText(bg)));
		if (!icon.isNull())
			it->setIcon(icon);
		it->setTextAlignment(Qt::AlignCenter);
		return it;
	};

	/* a scene tile shows its live thumbnail (cached now, or a colored
	   placeholder while the render timer catches up); off = no icon */
	auto sceneIcon = [&](const QString &uuid, const QColor &bg) -> QIcon {
		if (!thumbsOn)
			return QIcon();
		const QPixmap pm = thumbs::cached(uuid);
		if (!pm.isNull())
			return QIcon(pm);
		g_thumbPending << uuid;
		return thumbPlaceholder(bg);
	};

	if (!q.isEmpty()) {
		/* searching: flat matches from everywhere */
		for (const SceneRow &s : sceneRows()) {
			if (!s.name.contains(q, Qt::CaseInsensitive))
				continue;
			const QString hex = state().sceneColors ? state().colors.value(s.name) : QString();
			const QColor bg = hex.isEmpty() ? GRID_TILE_BG : QColor(hex);
			QListWidgetItem *it = addTile(ROLE_TYPE_SCENE, s.uuid, s.name, bg, sceneIcon(s.uuid, bg));
			if (s.uuid == curUuid)
				it->setSelected(true);
		}
		if (thumbsOn)
			startThumbTimer();
		g_applying = wasApplying;
		return;
	}

	if (!g_gridPath.isEmpty())
		addTile(ROLE_TYPE_UP, QString(), "Up", GRID_TILE_BG,
			g_grid->style()->standardIcon(QStyle::SP_FileDialogToParent));

	/* folders directly inside the current one */
	for (const QString &fpath : fd.order) {
		if (pathParent(fpath) != g_gridPath)
			continue;
		int count = 0;
		for (const QString &assigned : fd.assign)
			if (assigned == fpath)
				count++;
		const QString fhex = fd.colors.value(fpath);
		const QColor iconColor = fhex.isEmpty() ? NEUTRAL_FOLDER : QColor(fhex);
		addTile(ROLE_TYPE_FOLDER, fpath, QString("%1 (%2)").arg(pathName(fpath)).arg(count), GRID_TILE_BG,
			folderGlyph(iconColor));
	}

	/* scenes assigned right here (root = unassigned scenes) */
	for (const SceneRow &s : sceneRows()) {
		if (fd.assign.value(s.uuid) != g_gridPath)
			continue;
		if (g_gridPath.isEmpty() && fd.assign.contains(s.uuid))
			continue;
		const QString hex = state().sceneColors ? state().colors.value(s.name) : QString();
		const QColor bg = hex.isEmpty() ? GRID_TILE_BG : QColor(hex);
		QListWidgetItem *it = addTile(ROLE_TYPE_SCENE, s.uuid, s.name, bg, sceneIcon(s.uuid, bg));
		if (s.uuid == curUuid) {
			QFont f = it->font();
			f.setBold(true);
			it->setFont(f);
			it->setSelected(true);
		}
	}
	if (thumbsOn)
		startThumbTimer();
	g_applying = wasApplying;
}

static void gridClicked(QListWidgetItem *it)
{
	if (!it)
		return;
	const QString type = it->data(Qt::UserRole).toString();
	const QString key = it->data(Qt::UserRole + 1).toString();
	if (type == QLatin1String(ROLE_TYPE_UP)) {
		g_gridPath = pathParent(g_gridPath);
		rebuildGrid();
	} else if (type == QLatin1String(ROLE_TYPE_FOLDER)) {
		g_gridPath = key;
		rebuildGrid();
	} else if (type == QLatin1String(ROLE_TYPE_SCENE)) {
		if (obs_source_t *src = sceneByUuid(key)) {
			switchToScene(src);
			obs_source_release(src);
		}
	}
}

static void showGridMenu(const QPoint &pos)
{
	if (!g_grid)
		return;
	QListWidgetItem *it = g_grid->itemAt(pos);
	QMenu menu(g_grid);
	const QString type = it ? it->data(Qt::UserRole).toString() : QString();
	if (type == QLatin1String(ROLE_TYPE_SCENE)) {
		buildSceneMenu(menu, it->data(Qt::UserRole + 1).toString(), it->text());
	} else if (type == QLatin1String(ROLE_TYPE_FOLDER)) {
		const QString path = it->data(Qt::UserRole + 1).toString();
		menu.addAction("Open", [path]() {
			g_gridPath = path;
			rebuildGrid();
		});
		buildFolderMenu(menu, path);
	} else {
		menu.addAction("Add Scene...", []() { addScenePrompt(g_gridPath); });
		menu.addAction("New folder here", []() { newFolderPrompt(g_grid, QString(), g_gridPath); });
		menu.addAction("Sort all scenes A to Z", []() { sortScenesAtoZ(true, QString()); });
		if (state().sceneThumbs)
			menu.addAction("Refresh thumbnails", []() {
				thumbs::invalidateAll();
				rebuildGrid();
			});
	}
	menu.exec(g_grid->viewport()->mapToGlobal(pos));
}

static void updateViewMode()
{
	const bool grid = state().folderGridMode;
	if (g_tree)
		g_tree->setVisible(!grid);
	if (g_grid)
		g_grid->setVisible(grid);
	if (g_viewBtn) {
		g_viewBtn->setChecked(grid);
		g_viewBtn->setToolTip(grid ? "Switch to list" : "Switch to grid");
	}
	updateCrumb();
	if (grid)
		rebuildGrid();
}

/* ---- dock construction ---- */

void createDock()
{
	QWidget *panel = new QWidget();
	QVBoxLayout *v = new QVBoxLayout(panel);
	v->setContentsMargins(4, 4, 4, 4);
	v->setSpacing(4);

	/* search + view toggle + the small new folder button share one row */
	QHBoxLayout *topRow = new QHBoxLayout();
	topRow->setContentsMargins(0, 0, 0, 0);
	topRow->setSpacing(4);
	g_search = new QLineEdit(panel);
	g_search->setPlaceholderText("Search scenes");
	g_search->setClearButtonEnabled(true);
	QObject::connect(g_search, &QLineEdit::textChanged, panel, [](const QString &) { applySearch(); });
	topRow->addWidget(g_search, 1);
	g_viewBtn = new QToolButton(panel);
	g_viewBtn->setAutoRaise(true);
	g_viewBtn->setCheckable(true);
	g_viewBtn->setIcon(panel->style()->standardIcon(QStyle::SP_FileDialogContentsView));
	QObject::connect(g_viewBtn, &QToolButton::toggled, panel, [](bool on) {
		if (state().folderGridMode == on)
			return;
		state().folderGridMode = on;
		stateSave();
		updateViewMode();
	});
	topRow->addWidget(g_viewBtn);
	g_newBtn = new QToolButton(panel);
	g_newBtn->setAutoRaise(true);
	g_newBtn->setIcon(panel->style()->standardIcon(QStyle::SP_FileDialogNewFolder));
	g_newBtn->setToolTip("New folder");
	g_newBtn->setVisible(state().folderNewButton);
	QObject::connect(g_newBtn, &QToolButton::clicked, panel, [panel]() {
		newFolderPrompt(panel, QString(), state().folderGridMode ? g_gridPath : QString());
	});
	topRow->addWidget(g_newBtn);
	QToolButton *helpBtn = new QToolButton(panel);
	helpBtn->setAutoRaise(true);
	helpBtn->setText("?");
	helpBtn->setToolTip("DockX help (strmrx.com)");
	QObject::connect(helpBtn, &QToolButton::clicked, panel, []() { QDesktopServices::openUrl(QUrl(HELP_URL)); });
	topRow->addWidget(helpBtn);
	v->addLayout(topRow);

	g_crumb = new QLabel(panel);
	g_crumb->setVisible(false);
	v->addWidget(g_crumb);

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
	g_tree->setStyleSheet("QTreeWidget::item { min-height: 30px; padding-left: 2px; }"
			      "QTreeWidget::branch { background: transparent; border: none;"
			      " border-image: none; image: none; }");
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

	/* was the click that just fired on this row over its eye icon? */
	auto overRowEye = [](QTreeWidgetItem *it) {
		if (!g_tree)
			return false;
		const QPoint pos = g_tree->viewport()->mapFromGlobal(QCursor::pos());
		const QRect r = g_tree->visualItemRect(it);
		if (!r.contains(pos))
			return false;
		return pos.x() <= r.left() + g_tree->iconSize().width() + 4;
	};
	QObject::connect(g_tree, &QTreeWidget::itemClicked, g_tree, [overRowEye](QTreeWidgetItem *it, int) {
		if (isFolder(it)) {
			it->setExpanded(!it->isExpanded());
			return;
		}
		if (isSourceRow(it)) {
			/* the eye is a real button: click it to show/hide */
			if (!overRowEye(it))
				return;
			withRowItem(it, [](obs_sceneitem_t *item) {
				obs_sceneitem_set_visible(item, !obs_sceneitem_visible(item));
			});
			rebuildSoon();
			return;
		}
		if (!isScene(it))
			return;
		obs_source_t *src = obs_get_source_by_uuid(itemKey(it).toUtf8().constData());
		if (!src)
			return;
		switchToScene(src);
		obs_source_release(src);
	});
	QObject::connect(g_tree, &QTreeWidget::itemExpanded, g_tree, [](QTreeWidgetItem *it) {
		if (g_applying)
			return;
		if (isScene(it)) {
			g_expandedScenes.insert(itemKey(it));
			return;
		}
		if (!isFolder(it))
			return;
		data().collapsed.remove(itemKey(it));
		stateSave();
	});
	QObject::connect(g_tree, &QTreeWidget::itemCollapsed, g_tree, [](QTreeWidgetItem *it) {
		if (g_applying)
			return;
		if (isScene(it)) {
			g_expandedScenes.remove(itemKey(it));
			return;
		}
		if (!isFolder(it))
			return;
		data().collapsed.insert(itemKey(it));
		stateSave();
	});
	QObject::connect(g_tree, &QTreeWidget::itemDoubleClicked, g_tree, [overRowEye](QTreeWidgetItem *it, int) {
		/* double click a source row = show/hide, like the eye. Skip when the
		   double click is ON the eye: the single click already toggled */
		if (!isSourceRow(it) || overRowEye(it))
			return;
		withRowItem(it, [](obs_sceneitem_t *item) {
			obs_sceneitem_set_visible(item, !obs_sceneitem_visible(item));
		});
		rebuildSoon();
	});

	g_grid = new QListWidget(panel);
	g_grid->setViewMode(QListView::IconMode);
	g_grid->setFlow(QListView::LeftToRight);
	g_grid->setWrapping(true);
	g_grid->setResizeMode(QListView::Adjust);
	g_grid->setMovement(QListView::Static);
	g_grid->setSelectionMode(QAbstractItemView::SingleSelection);
	g_grid->setWordWrap(true);
	g_grid->setGridSize(QSize(112, 66));
	g_grid->setIconSize(QSize(20, 20));
	g_grid->setUniformItemSizes(true);
	QFont gridFont = g_grid->font();
	gridFont.setPointSizeF(gridFont.pointSizeF() + 0.5);
	g_grid->setFont(gridFont);
	g_grid->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(g_grid, &QListWidget::customContextMenuRequested, g_grid,
			 [](const QPoint &pos) { showGridMenu(pos); });
	QObject::connect(g_grid, &QListWidget::itemClicked, g_grid, [](QListWidgetItem *it) { gridClicked(it); });
	g_grid->setVisible(false);
	v->addWidget(g_grid, 1);

	if (!obs_frontend_add_dock_by_id("dockx_scene_folders", "Scene Folders", panel)) {
		obs_log(LOG_WARNING, "could not register the Scene Folders dock");
		delete panel;
		g_tree = nullptr;
		g_grid = nullptr;
		g_search = nullptr;
		g_newBtn = nullptr;
		g_viewBtn = nullptr;
		g_crumb = nullptr;
		return;
	}
	updateViewMode();
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
	if (g_thumbTimer)
		g_thumbTimer->stop();
	g_thumbPending.clear();
}

} // namespace folders
} // namespace dockx
