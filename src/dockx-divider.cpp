/*
DockX for OBS Studio (by StrmrX) -- drag handles for frozen dock boundaries.
GPL v2, see plugin-main.cpp for the full notice.

Qt's QMainWindow dock areas never trade space with each other directly, only
with the central widget between them. While DockX collapses the main preview
the central widget is pinned to zero size, so the boundary between opposite
dock areas (a left column against a right column) has no mediator: Qt's own
separator there freezes in both directions. This module finds exactly those
boundaries (two visible docked docks from DIFFERENT dock areas sitting edge
to edge) and lays a thin handle widget over each.

The drag itself (v0.46.12, SEVENTH design; 1-6 all failed on the rig):
- While the mouse is down NOTHING in the layout moves. The handle paints
  itself as a ghost bar and follows the mouse. (v0.45.1 forced a relayout
  per mouse move; every video dock's obs_display resized every few ms, the
  UI glitched, and the layout stormed. Never again.)
- THE 2026-09-10 LESSON, six designs deep: a visual column whose docks live
  in the TOP/BOTTOM bands has a DERIVED width -- Qt computes it as the
  leftover after the left/right areas and center take their share. Leftover
  space has no size of its own, so NOTHING can resize it: min=max pinning
  snapped back (0.45.1); cross-area resizeDocks is ignored (0.45.0);
  center-mediated resizeDocks gets re-balanced away (0.46.3-0.46.9);
  synthetic native separator drags find zero-length separators around the
  pinned 0x0 center (0.46.10); even patching the sizes inside a
  saveState blob gets re-derived on restore (0.46.11).
- v7 therefore fixes the CAUSE: when a drag hits a leftover column, DockX
  offers (once) to RE-DOCK that column into the real LEFT area -- same
  order, same tab groups, same sizes, snapshot taken first and rolled back
  if anything ends up wrong (repairLeftColumn). A real left area is a
  first-class citizen of the width equation, exactly like the right area
  (whose width provably obeys and persists). The trade then applies with a
  whole-edge resizeDocks inside TradeFlexScope.

Defensive by design: handles are plain widgets on top of the separator gap;
they intercept nothing else, never touch Qt layout internals, and are torn
down the moment the preview expands (Qt's native separators work again
then). Separators BETWEEN docks in the same area stay native and are never
covered. The center's 0x0 pin is restored on every exit path of the apply.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QLayout>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QTabBar>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace dockx {
namespace divider {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

static void rebuild();

/* the widest boundary gap worth bridging: two theme separators plus the
   thickest user separator setting, with slack */
static const int MAX_GAP = 28;
/* a handle thinner than this is unusable; grow it, centered on the gap */
static const int MIN_GRAB = 7;
/* never drag a dock smaller than this */
static const int MIN_DOCK = 40;

/* the strip between dock a and dock b, in main window coords; invalid when
   they are not edge to edge. Qt::Horizontal = a LEFT of b (widths change),
   Qt::Vertical = a ABOVE b (heights change) */
static QRect boundaryRect(const QRect &a, const QRect &b, Qt::Orientation o)
{
	if (o == Qt::Horizontal) {
		const int gap = b.left() - (a.right() + 1);
		if (gap < 0 || gap > MAX_GAP)
			return QRect();
		const int top = std::max(a.top(), b.top());
		const int bottom = std::min(a.bottom(), b.bottom());
		if (bottom - top < 20)
			return QRect();
		int x = a.right() + 1, w = gap;
		if (w < MIN_GRAB) {
			x -= (MIN_GRAB - w) / 2;
			w = MIN_GRAB;
		}
		return QRect(x, top, w, bottom - top + 1);
	}
	const int gap = b.top() - (a.bottom() + 1);
	if (gap < 0 || gap > MAX_GAP)
		return QRect();
	const int left = std::max(a.left(), b.left());
	const int right = std::min(a.right(), b.right());
	if (right - left < 20)
		return QRect();
	int y = a.bottom() + 1, h = gap;
	if (h < MIN_GRAB) {
		y -= (MIN_GRAB - h) / 2;
		h = MIN_GRAB;
	}
	return QRect(left, y, right - left + 1, h);
}

/* constraint relaxer for ONE WHOLE boundary trade, restored on scope exit.
   Two things must give for the trade to work, both learned on-rig 2026-09-10:
   1. The central widget's 0x0 pin must lift for the entire trade (re-pinning
      between the shrink and the grow forces Qt to empty the center again,
      handing the freed space straight back -- the net-zero failure).
   2. A dock AREA's usable range is derived from the docks inside it: its
      maximum along the trade axis is the SMALLEST maximum among its docks
      (QDockAreaLayoutInfo::maximumSize). One size-capped dock anywhere in
      the column freezes the whole column at that cap (the "settled at 304"
      failure). So every dock in BOTH affected areas gets its maximum lifted
      for the trade; minimums stay untouched (they protect content).
   Everything in the scope runs synchronously before Qt paints, so none of
   the relaxed states are ever visible. */
class TradeFlexScope {
public:
	TradeFlexScope(QMainWindow *m, QDockWidget *a, QDockWidget *b) : c(m->centralWidget())
	{
		centerPinned = c && c->maximumWidth() == 0 && c->maximumHeight() == 0;
		if (centerPinned) {
			c->setMinimumSize(0, 0);
			c->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		}
		const Qt::DockWidgetArea aa = m->dockWidgetArea(a);
		const Qt::DockWidgetArea ba = m->dockWidgetArea(b);
		const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QDockWidget *d : docks) {
			if (!d->isVisible() || d->isFloating())
				continue;
			const Qt::DockWidgetArea da = m->dockWidgetArea(d);
			if (da != aa && da != ba)
				continue;
			const QSize mx = d->maximumSize();
			if (mx.width() >= QWIDGETSIZE_MAX && mx.height() >= QWIDGETSIZE_MAX)
				continue; /* nothing to lift */
			saved.append({d, mx});
			d->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
		}
	}
	~TradeFlexScope()
	{
		for (const auto &s : saved)
			if (s.first)
				s.first->setMaximumSize(s.second);
		if (centerPinned) {
			c->setMinimumSize(0, 0);
			c->setMaximumSize(0, 0);
		}
	}

private:
	QWidget *c;
	bool centerPinned = false;
	QList<QPair<QPointer<QDockWidget>, QSize>> saved;
};

/* ---- topology repair (divider design v7) ----

   Re-dock a visual left column whose docks live in the TOP/BOTTOM bands
   (where Qt derives their width as leftover space and refuses every resize;
   see the header) into the real LEFT area: same top-to-bottom order, same
   tab groups, same sizes. A full layout snapshot is taken first and
   restored if any dock does not land where expected. */
static bool repairLeftColumn(QMainWindow *m, QList<QDockWidget *> col)
{
	if (col.isEmpty())
		return false;
	std::sort(col.begin(), col.end(), [](QDockWidget *a, QDockWidget *b) { return a->y() < b->y(); });
	const QByteArray before = m->saveState();
	const int width = col.first()->width();

	/* group tab sets: the visible member carries its hidden siblings */
	QList<QList<QDockWidget *>> groups;
	QList<int> heights;
	QSet<QDockWidget *> seen;
	for (QDockWidget *d : col) {
		if (seen.contains(d))
			continue;
		QList<QDockWidget *> grp{d};
		seen.insert(d);
		const auto tabbed = m->tabifiedDockWidgets(d);
		for (QDockWidget *t : tabbed) {
			if (!seen.contains(t)) {
				grp.append(t);
				seen.insert(t);
			}
		}
		groups.append(grp);
		heights.append(d->height());
	}

	/* pass 1: place the group MAINS as a clean vertical split chain --
	   tabifying is deferred so every split works on a single plain dock
	   (splitting into/around live tab groups is what merged Joey's whole
	   column into one tab group on the first rig run of this repair) */
	QDockWidget *prev = nullptr;
	QList<QDockWidget *> mains;
	for (const auto &grp : groups) {
		QDockWidget *main = grp.first();
		if (!prev)
			m->addDockWidget(Qt::LeftDockWidgetArea, main);
		else
			m->splitDockWidget(prev, main, Qt::Vertical);
		mains.append(main);
		prev = main;
	}
	if (m->layout())
		m->layout()->activate();

	/* pass 2: settle the geometry BEFORE re-attaching tabs */
	m->resizeDocks(mains, heights, Qt::Vertical);
	m->resizeDocks({mains.first()}, {width}, Qt::Horizontal);
	if (m->layout())
		m->layout()->activate();

	/* pass 3: put the tab siblings back onto their mains */
	for (const auto &grp : groups) {
		for (int i = 1; i < grp.size(); i++)
			m->tabifyDockWidget(grp.first(), grp[i]);
		grp.first()->raise(); /* keep the previously visible tab on top */
	}
	if (m->layout())
		m->layout()->activate();

	/* verify BOTH placement and structure: every dock in the left area,
	   and no two group MAINS merged into one tab set. Anything off = the
	   layout is not what the user had -> put it all back. */
	bool ok = true;
	for (const auto &grp : groups) {
		for (QDockWidget *d : grp)
			ok = ok && m->dockWidgetArea(d) == Qt::LeftDockWidgetArea;
		const auto tabs = m->tabifiedDockWidgets(grp.first());
		for (QDockWidget *t : tabs)
			ok = ok && !mains.contains(t);
	}
	if (!ok) {
		obs_log(LOG_WARNING, "divider: column repair did not reproduce the arrangement, rolling back");
		m->restoreState(before);
		if (m->layout())
			m->layout()->activate();
		return false;
	}
	obs_log(LOG_INFO, "divider: column repair re-docked %d dock group(s) into the left area", (int)groups.size());
	return true;
}

class BoundaryHandle : public QWidget {
public:
	QPointer<QDockWidget> first, second;
	Qt::Orientation orient = Qt::Horizontal;
	bool dragging = false;

	BoundaryHandle(QMainWindow *m, QDockWidget *a, QDockWidget *b, Qt::Orientation o, const QRect &r)
		: QWidget(m),
		  first(a),
		  second(b),
		  orient(o)
	{
		setObjectName("dockxBoundaryHandle");
		/* the widget paints nothing at rest (see paintEvent); no
		   stylesheet, the theme must not restyle it */
		setAttribute(Qt::WA_NoSystemBackground);
		setCursor(o == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
		setGeometry(r);
		show();
		raise();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		/* invisible at rest; while dragging, a soft ghost bar shows where
		   the boundary will land on release */
		if (!dragging)
			return;
		QPainter p(this);
		p.fillRect(rect(), QColor(140, 140, 140, 150));
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton || !first || !second) {
			e->ignore();
			return;
		}
		dragging = true;
		pressGlobal = e->globalPosition().toPoint();
		pressRect = geometry();
		firstSize = sizeOf(first);
		secondSize = sizeOf(second);
		update();
		e->accept();
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!dragging || !first || !second)
			return;
		/* layout untouched during the drag: only the ghost bar moves */
		const int a = targetFirstSize(e->globalPosition().toPoint());
		QRect r = pressRect;
		if (orient == Qt::Horizontal)
			r.moveLeft(pressRect.left() + (a - firstSize));
		else
			r.moveTop(pressRect.top() + (a - firstSize));
		setGeometry(r);
		update();
		e->accept();
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		/* dragging stays true THROUGH the apply: applyTrade pumps the event
		   loop (see dragNativeSeparator) and rebuild() must keep treating
		   this handle as live so it cannot delete us mid-apply */
		if (dragging && first && second)
			applyTrade(targetFirstSize(e->globalPosition().toPoint()));
		dragging = false;
		update();
		e->accept();
		QTimer::singleShot(0, [] { rebuild(); });
	}

private:
	int sizeOf(QDockWidget *d) const { return orient == Qt::Horizontal ? d->width() : d->height(); }

	int minOf(QDockWidget *d) const
	{
		const QSize h = d->minimumSizeHint();
		return std::max(MIN_DOCK, orient == Qt::Horizontal ? h.width() : h.height());
	}

	/* where the drag wants the first dock's size, clamped so both docks stay
	   at or above their true minimums (an honest ghost: it never shows a
	   position the release could not deliver) */
	int targetFirstSize(const QPoint &g) const
	{
		const int delta = orient == Qt::Horizontal ? g.x() - pressGlobal.x() : g.y() - pressGlobal.y();
		int a = firstSize + delta;
		a = std::max(a, minOf(first));
		a = std::min(a, firstSize + secondSize - minOf(second));
		return a;
	}

	void applyTrade(int newFirst)
	{
		QMainWindow *m = qobject_cast<QMainWindow *>(parentWidget());
		if (!m || newFirst == firstSize)
			return;
		const bool horiz = orient == Qt::Horizontal;

		/* classify every dock by which side of the dragged boundary its
		   edge sits on (whole-edge semantics: a visual column can span
		   multiple Qt areas, so the trade must cover every aligned dock) */
		const int edgeA = horiz ? first->geometry().right() : first->geometry().bottom();
		const int edgeB = horiz ? second->geometry().left() : second->geometry().top();
		QList<QDockWidget *> sideA, sideB;
		const auto all = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QDockWidget *d : all) {
			if (!d->isVisible() || d->isFloating() || m->dockWidgetArea(d) == Qt::NoDockWidgetArea)
				continue;
			const QRect g = d->geometry();
			if (std::abs((horiz ? g.right() : g.bottom()) - edgeA) <= MAX_GAP)
				sideA.append(d);
			else if (std::abs((horiz ? g.left() : g.top()) - edgeB) <= MAX_GAP)
				sideB.append(d);
		}

		/* leftover-column detection (see header): near-side docks living in
		   the TOP/BOTTOM bands of a horizontal drag have a derived width no
		   API can change. Offer the real fix ONCE: re-dock the column. */
		if (horiz && first->geometry().left() <= MAX_GAP) {
			bool derived = false;
			for (QDockWidget *d : sideA) {
				const Qt::DockWidgetArea a = m->dockWidgetArea(d);
				derived = derived || a == Qt::TopDockWidgetArea || a == Qt::BottomDockWidgetArea;
			}
			if (derived) {
				static bool declined = false;
				if (declined)
					return;
				const auto ans = QMessageBox::question(
					m, "DockX",
					"This column is docked in a spot OBS cannot resize (a quirk of how "
					"it got arranged). DockX can re-dock it properly: it will look the "
					"same, keep its tabs and sizes, and this divider will work.\n\n"
					"Fix the column now?",
					QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
				if (ans != QMessageBox::Yes) {
					declined = true;
					return;
				}
				TradeFlexScope flex(m, first, second);
				if (!repairLeftColumn(m, sideA))
					return;
			}
		}

		/* the trade: every dock on the edge, one resizeDocks call, center
		   flexible for the duration */
		const int delta = newFirst - firstSize;
		QList<QDockWidget *> docks;
		QList<int> sizes;
		for (QDockWidget *d : sideA) {
			docks.append(d);
			sizes.append(std::max(MIN_DOCK, (horiz ? d->width() : d->height()) + delta));
		}
		for (QDockWidget *d : sideB) {
			docks.append(d);
			sizes.append(std::max(MIN_DOCK, (horiz ? d->width() : d->height()) - delta));
		}
		{
			TradeFlexScope flex(m, first, second);
			m->resizeDocks(docks, sizes, orient);
			if (m->layout())
				m->layout()->activate();
		} /* dock maximums + the center's 0x0 pin restored here */
		if (m->layout())
			m->layout()->activate();
		const int got = sizeOf(first);
		if (std::abs(got - newFirst) > 4) {
			obs_log(LOG_WARNING,
				"divider: drag wanted %d, layout settled at %d (%d docks traded; a dock minimum, or Qt refused)",
				newFirst, got, (int)docks.size());
			logAreaDiagnostics(m);
			/* if a dock minimum is the wall, SAY which (blocker hint) */
			const bool horiz = orient == Qt::Horizontal;
			QDockWidget *clamp = nullptr;
			int clampMin = -1;
			for (QDockWidget *d : docks) {
				const int size = horiz ? d->width() : d->height();
				const int eff =
					std::max(horiz ? d->minimumWidth() : d->minimumHeight(),
						 horiz ? d->minimumSizeHint().width() : d->minimumSizeHint().height());
				if (size <= eff + 6 && eff > clampMin) {
					clampMin = eff;
					clamp = d;
				}
			}
			if (clamp)
				blocker::flashBlocked(clamp);
		}
		/* follow the real boundary so the cursor stays on the handle */
		const QRect r = boundaryRect(first->geometry(), second->geometry(), orient);
		if (r.isValid())
			setGeometry(r);
	}

	/* a failed trade means the layout math is not what we think; print the
	   full topology -- window, corners, and every dock in the two areas with
	   position, size, and constraints -- so one log read shows the real
	   shape of the layout */
	void logAreaDiagnostics(QMainWindow *m)
	{
		const Qt::DockWidgetArea aa = m->dockWidgetArea(first);
		const Qt::DockWidgetArea ba = m->dockWidgetArea(second);
		const bool horiz = orient == Qt::Horizontal;
		obs_log(LOG_WARNING,
			"divider:   window %dx%d, corners TL=%d TR=%d BL=%d BR=%d, first='%s' (area %d), "
			"second='%s' (area %d), %s drag",
			m->width(), m->height(), (int)m->corner(Qt::TopLeftCorner), (int)m->corner(Qt::TopRightCorner),
			(int)m->corner(Qt::BottomLeftCorner), (int)m->corner(Qt::BottomRightCorner),
			first->objectName().toUtf8().constData(), (int)aa, second->objectName().toUtf8().constData(),
			(int)ba, horiz ? "horizontal" : "vertical");
		const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QDockWidget *d : docks) {
			if (!d->isVisible() || d->isFloating())
				continue;
			const Qt::DockWidgetArea da = m->dockWidgetArea(d);
			if (da != aa && da != ba)
				continue;
			const QSize mn = d->minimumSize(), mh = d->minimumSizeHint(), mx = d->maximumSize();
			const QRect g = d->geometry();
			obs_log(LOG_WARNING, "divider:   area %d dock '%s' at %d,%d %dx%d min=%d minHint=%d max=%d",
				(int)da, d->objectName().toUtf8().constData(), g.x(), g.y(), g.width(), g.height(),
				horiz ? mn.width() : mn.height(), horiz ? mh.width() : mh.height(),
				horiz ? mx.width() : mx.height());
		}
	}

	QPoint pressGlobal;
	QRect pressRect;
	int firstSize = 0, secondSize = 0;
};

static QList<QPointer<BoundaryHandle>> g_handles;
static QTimer *g_timer = nullptr;

static void clearHandles()
{
	for (auto &h : g_handles)
		if (h)
			h->deleteLater();
	g_handles.clear();
}

struct Spec {
	QRect rect;
	QDockWidget *a;
	QDockWidget *b;
	Qt::Orientation o;
};

static void rebuild()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	if (!g_timer || !g_timer->isActive())
		return; /* deactivated since this was queued */
	for (auto &h : g_handles)
		if (h && h->dragging)
			return; /* never yank the handle mid drag */

	QList<QDockWidget *> live;
	bool hasLeft = false, hasRight = false, hasTop = false, hasBottom = false;
	const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *d : docks) {
		if (d->isVisible() && !d->isFloating() && d->width() > 0 &&
		    m->dockWidgetArea(d) != Qt::NoDockWidgetArea) {
			live.push_back(d);
			const Qt::DockWidgetArea a = m->dockWidgetArea(d);
			hasLeft = hasLeft || a == Qt::LeftDockWidgetArea;
			hasRight = hasRight || a == Qt::RightDockWidgetArea;
			hasTop = hasTop || a == Qt::TopDockWidgetArea;
			hasBottom = hasBottom || a == Qt::BottomDockWidgetArea;
		}
	}

	/* while curing Joey's leftover column (2026-09-10) we learned Qt's OWN
	   between-area separator drags work fine ACROSS the pinned 0x0 center,
	   as long as both flanking areas are populated (the drag pushes space
	   straight through the zero-width center). When natives work they feel
	   better than the ghost bar -- so DockX only bridges seams Qt cannot
	   serve: a horizontal seam with the left or right area empty (the
	   leftover-column case, where the handle's real job is offering the
	   column repair), and the vertical mirror. */
	const bool nativeH = hasLeft && hasRight;
	const bool nativeV = hasTop && hasBottom;

	QList<Spec> specs;
	for (QDockWidget *a : live) {
		for (QDockWidget *b : live) {
			if (a == b || m->dockWidgetArea(a) == m->dockWidgetArea(b))
				continue;
			if (!nativeH) {
				QRect r = boundaryRect(a->geometry(), b->geometry(), Qt::Horizontal);
				if (r.isValid())
					specs.append({r, a, b, Qt::Horizontal});
			}
			if (!nativeV) {
				QRect r = boundaryRect(a->geometry(), b->geometry(), Qt::Vertical);
				if (r.isValid())
					specs.append({r, a, b, Qt::Vertical});
			}
		}
	}

	/* long tab rows set a hard floor on a column's width (each label claims
	   its full text). Elided labels + scroll buttons let tab bars shrink,
	   so a tabbed column can go as narrow as its widest dock instead of the
	   sum of its tab labels (Joey: dropping from 4 tabs to 3 visibly
	   lowered his column's minimum). Idempotent; runs on the same timer. */
	const auto tabBars = m->findChildren<QTabBar *>(QString(), Qt::FindDirectChildrenOnly);
	for (QTabBar *tb : tabBars) {
		if (tb->elideMode() != Qt::ElideRight) {
			tb->setElideMode(Qt::ElideRight);
			tb->setUsesScrollButtons(true);
		}
	}

	/* unchanged = leave the live widgets alone */
	if (specs.size() == g_handles.size()) {
		bool same = true;
		for (int i = 0; i < specs.size(); i++) {
			BoundaryHandle *h = g_handles[i];
			if (!h || h->geometry() != specs[i].rect || h->first != specs[i].a || h->second != specs[i].b ||
			    h->orient != specs[i].o) {
				same = false;
				break;
			}
		}
		if (same)
			return;
	}

	clearHandles();
	for (const Spec &s : specs)
		g_handles.append(new BoundaryHandle(m, s.a, s.b, s.o, s.rect));

	static int lastCount = -1;
	if (specs.size() != lastCount) {
		lastCount = (int)specs.size();
		obs_log(LOG_INFO, "divider: bridging %d frozen dock boundar%s", lastCount,
			lastCount == 1 ? "y" : "ies");
	}
}

void setActive(bool on)
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	if (on) {
		if (!g_timer) {
			g_timer = new QTimer(m);
			g_timer->setInterval(400);
			QObject::connect(g_timer, &QTimer::timeout, [] { rebuild(); });
		}
		if (!g_timer->isActive()) {
			g_timer->start();
			rebuild();
		}
	} else {
		if (g_timer)
			g_timer->stop();
		clearHandles();
	}
}

void shutdown()
{
	setActive(false);
}

} // namespace divider
} // namespace dockx
