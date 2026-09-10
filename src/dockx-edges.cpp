/*
DockX for OBS Studio (by StrmrX) -- edge row span controls.
GPL v2, see plugin-main.cpp for the full notice.

Two features, both about the top and bottom strips of the window:

1. CORNER OWNERSHIP. Whether a top/bottom dock row runs the full window
   width or stops at the side columns is decided by QMainWindow's four
   corner assignments. OBS exposes only an all-or-nothing "Full-height
   docks" menu toggle (all four corners to the sides). DockX stores a
   per-edge preference (0 = let OBS decide, 1 = the edge row spans the
   full window, 2 = the side columns keep that edge's corners) and
   reasserts it on a slow timer, because OBS rewrites all four corners
   whenever its own toggle is used.

2. STRETCH A DOCK ACROSS A ROW. "Put the mixer under exactly my two chat
   docks": rebuild the dock nesting so one dock becomes a strip spanning
   a chosen row of neighbor docks, above or below them. Reuses the
   machinery proven by the v0.46.12 column repair: snapshot with
   saveState first, split the plain docks in a safe order (tabify LAST,
   the v0.46.13 lesson), settle sizes, re-attach tab siblings, verify
   both placement and structure, and roll the snapshot back if the
   result is not what was asked for. Reachable from a right-click on any
   dock title bar (added to OBS's own dock menu) and from Tools > DockX.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace dockx {
namespace edges {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* ---- corner ownership ---- */

/* the corner layout OBS itself chose, captured before DockX's first apply,
   so unticking a full width box hands that edge straight back instead of
   leaving DockX's override stuck until the next OBS restart */
static Qt::DockWidgetArea g_baseCorner[4]; /* indexed by Qt::Corner */
static bool g_baseKnown = false;

void releaseEdge(bool top)
{
	QMainWindow *m = mainWindow();
	if (!m || !g_baseKnown)
		return;
	if (top) {
		m->setCorner(Qt::TopLeftCorner, g_baseCorner[Qt::TopLeftCorner]);
		m->setCorner(Qt::TopRightCorner, g_baseCorner[Qt::TopRightCorner]);
	} else {
		m->setCorner(Qt::BottomLeftCorner, g_baseCorner[Qt::BottomLeftCorner]);
		m->setCorner(Qt::BottomRightCorner, g_baseCorner[Qt::BottomRightCorner]);
	}
}

void applyCorners()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	const int top = state().edgeTop;
	const int bottom = state().edgeBottom;
	if (top == 1) {
		m->setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		m->setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
	} else if (top == 2) {
		m->setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		m->setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
	}
	if (bottom == 1) {
		m->setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		m->setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	} else if (bottom == 2) {
		m->setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		m->setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	}
}

/* true when a corner sits where the user's explicit preference says it
   must; "let OBS decide" (0) matches anything */
static bool cornersMatch(QMainWindow *m)
{
	const int top = state().edgeTop;
	const int bottom = state().edgeBottom;
	if (top == 1 && (m->corner(Qt::TopLeftCorner) != Qt::TopDockWidgetArea ||
			 m->corner(Qt::TopRightCorner) != Qt::TopDockWidgetArea))
		return false;
	if (top == 2 && (m->corner(Qt::TopLeftCorner) != Qt::LeftDockWidgetArea ||
			 m->corner(Qt::TopRightCorner) != Qt::RightDockWidgetArea))
		return false;
	if (bottom == 1 && (m->corner(Qt::BottomLeftCorner) != Qt::BottomDockWidgetArea ||
			    m->corner(Qt::BottomRightCorner) != Qt::BottomDockWidgetArea))
		return false;
	if (bottom == 2 && (m->corner(Qt::BottomLeftCorner) != Qt::LeftDockWidgetArea ||
			    m->corner(Qt::BottomRightCorner) != Qt::RightDockWidgetArea))
		return false;
	return true;
}

/* ---- the stretch itself ----

   Rebuild the nesting so `strip` spans `targets` (left to right) as one
   full-width row above or below them. Qt's tree is built by insertion
   order: docking the strip below the first target creates a vertical
   pair, then splitting the following targets HORIZONTALLY off the first
   nests them all inside the pair's other cell -- so the strip's edge ends
   up spanning the whole row. Snapshot first, verify after, roll back on
   any mismatch. */
static bool stretchDock(QMainWindow *m, QDockWidget *strip, QList<QDockWidget *> targets, bool below)
{
	if (!m || !strip || targets.isEmpty())
		return false;
	targets.removeAll(strip);
	if (targets.isEmpty())
		return false;
	std::sort(targets.begin(), targets.end(),
		  [](QDockWidget *a, QDockWidget *b) { return a->x() != b->x() ? a->x() < b->x() : a->y() < b->y(); });

	/* two checked docks tabbed together are ONE column; the first keeps
	   the spot and carries the group */
	{
		QList<QDockWidget *> kept;
		for (QDockWidget *t : targets) {
			bool dup = false;
			for (QDockWidget *k : kept)
				dup = dup || m->tabifiedDockWidgets(k).contains(t);
			if (!dup)
				kept.append(t);
		}
		targets = kept;
	}

	{
		QString names;
		for (QDockWidget *t : targets)
			names += (names.isEmpty() ? QString() : QString(", ")) + t->objectName();
		obs_log(LOG_INFO, "edges: stretching '%s' %s [%s]", strip->objectName().toUtf8().constData(),
			below ? "under" : "over", names.toUtf8().constData());
	}

	const QByteArray before = m->saveState();
	const int stripHeight = strip->height();
	const int anchorHeight = targets.first()->height();
	QList<int> widths;
	for (QDockWidget *t : targets)
		widths.append(t->width());

	/* moved docks bring their tab siblings along (a tab group reads as
	   one column); the STRIP leaves its siblings behind on purpose --
	   stretching one dock out of a tab group is the whole point of the
	   gesture. Captured before anything moves. */
	QList<QPair<QDockWidget *, QList<QDockWidget *>>> sibs;
	for (int i = 1; i < targets.size(); i++) {
		QList<QDockWidget *> s;
		const auto tabbed = m->tabifiedDockWidgets(targets[i]);
		for (QDockWidget *t : tabbed)
			if (t != strip && !targets.contains(t))
				s.append(t);
		if (!s.isEmpty())
			sibs.append({targets[i], s});
	}

	/* the ANCHOR is different: it never moves, and every split targets it.
	   Qt quirk (documented on splitDockWidget): when `after` sits in a tab
	   group the call ADDS the moved dock to that group instead of
	   splitting -- a tabbed anchor turned Joey's whole build into one tab
	   pile and forced the rollback. So the anchor must be PLAIN before
	   pass 1: park its tab siblings at the area edge (an addDockWidget
	   move, same call the column repair trusts; nothing paints mid
	   rebuild) and re-attach them in pass 3 like every other group. */
	QList<QDockWidget *> anchorSibs = m->tabifiedDockWidgets(targets.first());
	{
		Qt::DockWidgetArea parkArea = m->dockWidgetArea(targets.first());
		if (parkArea == Qt::NoDockWidgetArea)
			parkArea = Qt::RightDockWidgetArea;
		for (QDockWidget *s : anchorSibs)
			m->addDockWidget(parkArea, s);
	}

	/* constraint relaxer, same two lessons as the divider's
	   TradeFlexScope: the pinned 0x0 center and any size-capped dock can
	   both silently veto the resize pass. Lifted for the rebuild,
	   restored before anything paints. */
	QWidget *center = m->centralWidget();
	const bool centerPinned = center && center->maximumWidth() == 0 && center->maximumHeight() == 0;
	if (centerPinned)
		center->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
	QList<QPair<QPointer<QDockWidget>, QSize>> lifted;
	const auto all = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *d : all) {
		const QSize mx = d->maximumSize();
		if (mx.width() < QWIDGETSIZE_MAX || mx.height() < QWIDGETSIZE_MAX) {
			lifted.append({d, mx});
			d->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		}
	}
	const auto restoreCaps = [&]() {
		for (const auto &s : lifted)
			if (s.first)
				s.first->setMaximumSize(s.second);
		if (centerPinned)
			center->setMaximumSize(0, 0);
	};

	/* pass 1: structure. Everything splits on PLAIN docks; tab groups are
	   reattached last (splitting into live tab groups is what merged a
	   whole column into one tab set on the repair's first rig run). */
	QDockWidget *anchor = targets.first();
	m->splitDockWidget(anchor, strip, Qt::Vertical);
	if (!below) /* re-dock the anchor below the strip: the pair flips */
		m->splitDockWidget(strip, anchor, Qt::Vertical);
	QDockWidget *prev = anchor;
	for (int i = 1; i < targets.size(); i++) {
		m->splitDockWidget(prev, targets[i], Qt::Horizontal);
		prev = targets[i];
	}
	if (m->layout())
		m->layout()->activate();

	/* pass 2: sizes. The strip's width is derived from the row, so only
	   heights and the row's column widths need settling. */
	m->resizeDocks({strip}, {stripHeight}, Qt::Vertical);
	m->resizeDocks({anchor}, {anchorHeight}, Qt::Vertical);
	m->resizeDocks(targets, widths, Qt::Horizontal);
	if (m->layout())
		m->layout()->activate();

	/* pass 3: tab siblings back onto their columns (the anchor's parked
	   ones first; the strip may have been one of them and stays out) */
	for (QDockWidget *s : anchorSibs)
		if (s != strip && !targets.contains(s))
			m->tabifyDockWidget(anchor, s);
	if (!anchorSibs.isEmpty())
		anchor->raise();
	for (const auto &s : sibs) {
		for (QDockWidget *t : s.second)
			m->tabifyDockWidget(s.first, t);
		s.first->raise();
	}
	if (m->layout())
		m->layout()->activate();
	restoreCaps();

	/* pass 4: verify placement AND structure; anything off = not the
	   arrangement the user asked for -> snapshot goes back */
	bool ok = true;
	const Qt::DockWidgetArea area = m->dockWidgetArea(anchor);
	ok = ok && area != Qt::NoDockWidgetArea && m->dockWidgetArea(strip) == area;
	int minLeft = INT_MAX, maxRight = INT_MIN, minTop = INT_MAX, maxBottom = INT_MIN;
	for (QDockWidget *t : targets) {
		ok = ok && m->dockWidgetArea(t) == area;
		const QRect g = t->geometry();
		minLeft = std::min(minLeft, g.left());
		maxRight = std::max(maxRight, g.right());
		minTop = std::min(minTop, g.top());
		maxBottom = std::max(maxBottom, g.bottom());
	}
	const QRect sg = strip->geometry();
	ok = ok && sg.left() <= minLeft + 32 && sg.right() >= maxRight - 32;
	ok = ok && (below ? sg.top() >= maxBottom - 8 : sg.bottom() <= minTop + 8);
	const auto stripTabs = m->tabifiedDockWidgets(strip);
	for (QDockWidget *t : targets) {
		ok = ok && !stripTabs.contains(t);
		const auto tabs = m->tabifiedDockWidgets(t);
		for (QDockWidget *u : targets)
			ok = ok && (u == t || !tabs.contains(u));
	}
	if (!ok) {
		obs_log(LOG_WARNING, "edges: stretch of '%s' across %d dock(s) did not come together, rolling back",
			strip->objectName().toUtf8().constData(), (int)targets.size());
		m->restoreState(before);
		if (m->layout())
			m->layout()->activate();
		return false;
	}
	/* a stretch that WORKED but was not what the user meant needs a way
	   back too: the Layouts tab's Undo apply restores this snapshot */
	state().undoState = before;
	obs_log(LOG_INFO, "edges: stretched '%s' %s a row of %d dock(s)", strip->objectName().toUtf8().constData(),
		below ? "under" : "over", (int)targets.size());
	return true;
}

/* visible, real docks a strip could span (or be); background tabs are
   represented by their visible group main, exactly like the repair */
static QList<QDockWidget *> candidateDocks(QMainWindow *m)
{
	QList<QDockWidget *> out;
	const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *d : docks)
		if (d->isVisible() && !d->isFloating() && m->dockWidgetArea(d) != Qt::NoDockWidgetArea)
			out.append(d);
	std::sort(out.begin(), out.end(),
		  [](QDockWidget *a, QDockWidget *b) { return a->x() != b->x() ? a->x() < b->x() : a->y() < b->y(); });
	return out;
}

void showStretchDialog(QDockWidget *dock, QWidget *parent)
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;

	QDialog dlg(parent ? parent : m);
	dlg.setWindowTitle("Stretch a dock");
	QVBoxLayout *v = new QVBoxLayout(&dlg);

	/* one flow, always the same: WHICH dock, ACROSS which docks, WHERE.
	   The right-click path lands here too, with its dock preselected --
	   Joey's first run never made clear which dock was being stretched */
	v->addWidget(new QLabel("Stretch this dock:", &dlg));
	QComboBox *dockPick = new QComboBox(&dlg);
	int preselect = 0;
	{
		const auto cands = candidateDocks(m);
		for (QDockWidget *d : cands) {
			if (d == dock)
				preselect = dockPick->count();
			dockPick->addItem(d->windowTitle(), QVariant::fromValue((void *)d));
		}
		/* a floating dock right-clicked to get here is not in the
		   candidate list; put it on top so the choice is honored */
		if (dock && (cands.isEmpty() || !cands.contains(dock))) {
			dockPick->insertItem(0, dock->windowTitle(), QVariant::fromValue((void *)dock));
			preselect = 0;
		}
	}
	dockPick->setCurrentIndex(preselect);
	v->addWidget(dockPick);

	v->addWidget(new QLabel("Across these docks:", &dlg));
	QListWidget *list = new QListWidget(&dlg);
	auto pickedStrip = [dockPick]() -> QDockWidget * {
		return (QDockWidget *)dockPick->currentData().value<void *>();
	};
	auto reloadList = [list, pickedStrip, m]() {
		QDockWidget *strip = pickedStrip();
		list->clear();
		for (QDockWidget *d : candidateDocks(m)) {
			if (d == strip)
				continue;
			QListWidgetItem *it = new QListWidgetItem(d->windowTitle(), list);
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			it->setCheckState(Qt::Unchecked);
			it->setData(Qt::UserRole, QVariant::fromValue((void *)d));
		}
	};
	reloadList();
	v->addWidget(list, 1);

	QHBoxLayout *dr = new QHBoxLayout();
	QRadioButton *underBtn = new QRadioButton("Under them", &dlg);
	QRadioButton *aboveBtn = new QRadioButton("Above them", &dlg);
	underBtn->setChecked(true);
	dr->addWidget(underBtn);
	dr->addWidget(aboveBtn);
	dr->addStretch(1);
	v->addLayout(dr);

	/* the summary says, in plain words, exactly what the button will do;
	   the button stays off until the sentence makes sense */
	QLabel *summary = new QLabel(&dlg);
	summary->setWordWrap(true);
	v->addWidget(summary);

	QDialogButtonBox *bb = new QDialogButtonBox(&dlg);
	QPushButton *goBtn = bb->addButton("Stretch", QDialogButtonBox::AcceptRole);
	bb->addButton(QDialogButtonBox::Cancel);
	v->addWidget(bb);
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

	auto checkedNames = [list]() {
		QStringList names;
		for (int i = 0; i < list->count(); i++)
			if (list->item(i)->checkState() == Qt::Checked)
				names.append(list->item(i)->text());
		return names;
	};
	auto refreshSummary = [summary, goBtn, dockPick, underBtn, checkedNames]() {
		const QStringList names = checkedNames();
		if (names.isEmpty()) {
			summary->setText("Tick at least one dock in the list above.");
			goBtn->setEnabled(false);
			return;
		}
		summary->setText(QString("\"%1\" becomes one wide strip %2 %3. Undo apply on the "
					 "Layouts tab reverses it.")
					 .arg(dockPick->currentText(), underBtn->isChecked() ? "under" : "above",
					      names.join(" + ")));
		goBtn->setEnabled(true);
	};
	refreshSummary();
	QObject::connect(dockPick, &QComboBox::currentIndexChanged, &dlg, [reloadList, refreshSummary]() {
		reloadList();
		refreshSummary();
	});
	QObject::connect(list, &QListWidget::itemChanged, &dlg,
			 [refreshSummary](QListWidgetItem *) { refreshSummary(); });
	QObject::connect(underBtn, &QRadioButton::toggled, &dlg, [refreshSummary](bool) { refreshSummary(); });

	if (dlg.exec() != QDialog::Accepted)
		return;

	QDockWidget *strip = pickedStrip();
	QList<QDockWidget *> targets;
	for (int i = 0; i < list->count(); i++) {
		QListWidgetItem *it = list->item(i);
		if (it->checkState() == Qt::Checked)
			targets.append((QDockWidget *)it->data(Qt::UserRole).value<void *>());
	}
	if (!strip || targets.isEmpty())
		return; /* the Stretch button is disabled in this state anyway */
	if (!stretchDock(m, strip, targets, underBtn->isChecked()))
		QMessageBox::information(parent ? parent : (QWidget *)m, "DockX",
					 "That arrangement did not come together, so your docks were put "
					 "back exactly as they were. The OBS log has the details.");
}

/* ---- dock title bar right-click: add the stretch entry to OBS's own
        dock menu (clicks inside a dock's CONTENT keep their own menus) ---- */

class TitleMenuFilter : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		if (e->type() != QEvent::ContextMenu)
			return false;
		QDockWidget *d = qobject_cast<QDockWidget *>(o);
		QMainWindow *m = mainWindow();
		if (!d || !m)
			return false;
		auto *ce = static_cast<QContextMenuEvent *>(e);
		if (d->widget() && d->widget()->geometry().contains(ce->pos()))
			return false; /* content area, not the title bar */
		QMenu *menu = m->createPopupMenu();
		if (!menu)
			menu = new QMenu(d);
		menu->addSeparator();
		QAction *act = menu->addAction(QString("Stretch \"%1\" across a row...").arg(d->windowTitle()));
		QPointer<QDockWidget> dp(d);
		QObject::connect(act, &QAction::triggered, d, [dp]() {
			if (dp)
				showStretchDialog(dp, nullptr);
		});
		menu->exec(ce->globalPos());
		menu->deleteLater();
		return true;
	}
};

static QTimer *g_timer = nullptr;
static TitleMenuFilter *g_filter = nullptr;

static void tick()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	/* OBS's own Full-height docks toggle rewrites all four corners; put
	   the user's explicit per-edge choices back */
	if ((state().edgeTop || state().edgeBottom) && !cornersMatch(m)) {
		applyCorners();
		obs_log(LOG_INFO, "edges: reasserted edge row corner ownership (OBS changed it)");
	}
	/* hook the stretch menu onto any dock we have not seen yet (docks
	   come and go with collections and DockX's own features) */
	const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *d : docks) {
		if (!d->property("dockxStretchHook").toBool()) {
			d->setProperty("dockxStretchHook", true);
			d->installEventFilter(g_filter);
		}
	}
}

void start()
{
	QMainWindow *m = mainWindow();
	if (!m || g_timer)
		return;
	g_filter = new TitleMenuFilter(m);
	g_baseCorner[Qt::TopLeftCorner] = m->corner(Qt::TopLeftCorner);
	g_baseCorner[Qt::TopRightCorner] = m->corner(Qt::TopRightCorner);
	g_baseCorner[Qt::BottomLeftCorner] = m->corner(Qt::BottomLeftCorner);
	g_baseCorner[Qt::BottomRightCorner] = m->corner(Qt::BottomRightCorner);
	g_baseKnown = true;
	applyCorners();
	g_timer = new QTimer(m);
	g_timer->setInterval(1500);
	QObject::connect(g_timer, &QTimer::timeout, [] { tick(); });
	g_timer->start();
	tick();
}

void shutdown()
{
	if (g_timer) {
		g_timer->stop();
		g_timer = nullptr; /* owned by the main window, dies with it */
	}
	g_filter = nullptr;
}

} // namespace edges
} // namespace dockx
