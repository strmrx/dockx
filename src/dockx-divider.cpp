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

The drag itself (v0.46.0, third design -- the first two failed on the rig):
- While the mouse is down NOTHING in the layout moves. The handle paints
  itself as a ghost bar and follows the mouse. (v0.45.1 forced a relayout
  per mouse move; every video dock's obs_display resized every few ms, the
  UI glitched, and the layout stormed. Never again.)
- On release the trade is applied ONCE, routed through the center: the
  central widget's 0x0 pin is lifted for one moment, the shrinking dock is
  resized first (the center absorbs the space), the growing dock second
  (the center gives it straight back), then the center is pinned to 0x0
  again. Both calls are QMainWindow::resizeDocks trading with the CENTER,
  which is the one trade it never refuses (proven on-rig 2026-09-09: a
  direct cross-area resizeDocks is silently ignored). resizeDocks writes
  the result into Qt's own dock layout state, so the new sizes stick --
  unlike the v0.45.1 min=max pin, which Qt reverted on release (the
  snap-back).

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
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
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
		const bool apply = dragging && first && second;
		dragging = false;
		update();
		if (apply)
			applyTrade(targetFirstSize(e->globalPosition().toPoint()));
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
		const int delta = newFirst - firstSize;

		/* resize the WHOLE EDGE, not just the two docks under the handle.
		   Rig lesson (2026-09-10): a visual column can span MULTIPLE Qt
		   dock areas (Joey's left column = Controls/stream info in the
		   LEFT area stacked over stats/preview in the BOTTOM area). Qt
		   only takes width orders through some areas (left/right for a
		   horizontal drag); top/bottom area widths are derived. Sending
		   the same trade to EVERY dock whose edge sits on this boundary
		   means the order lands through the commandable areas, and the
		   derived ones follow automatically. */
		const bool horiz = orient == Qt::Horizontal;
		const int edgeA = horiz ? first->geometry().right() : first->geometry().bottom();
		const int edgeB = horiz ? second->geometry().left() : second->geometry().top();
		QList<QDockWidget *> docks;
		QList<int> sizes;
		const auto all = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QDockWidget *d : all) {
			if (!d->isVisible() || d->isFloating() || m->dockWidgetArea(d) == Qt::NoDockWidgetArea)
				continue;
			const QRect g = d->geometry();
			const int dEnd = horiz ? g.right() : g.bottom();
			const int dStart = horiz ? g.left() : g.top();
			const int dSize = horiz ? g.width() : g.height();
			if (std::abs(dEnd - edgeA) <= MAX_GAP) { /* before the boundary: grows with first */
				docks.append(d);
				sizes.append(std::max(MIN_DOCK, dSize + delta));
			} else if (std::abs(dStart - edgeB) <= MAX_GAP) { /* after: shrinks as first grows */
				docks.append(d);
				sizes.append(std::max(MIN_DOCK, dSize - delta));
			}
		}
		{
			TradeFlexScope flex(m, first, second);
			/* one call for the whole edge: Qt routes the space through
			   the (briefly flexible) center */
			m->resizeDocks(docks, sizes, orient);
			if (m->layout())
				m->layout()->activate();
			if (std::abs(sizeOf(first) - newFirst) > 4) {
				/* refused in one go: apply the shrinking side first
				   (the center absorbs the space), the growing side
				   second (the center hands it back) */
				QList<QDockWidget *> shD, grD;
				QList<int> shS, grS;
				for (int i = 0; i < docks.size(); i++) {
					const int cur = horiz ? docks[i]->width() : docks[i]->height();
					if (sizes[i] <= cur) {
						shD.append(docks[i]);
						shS.append(sizes[i]);
					} else {
						grD.append(docks[i]);
						grS.append(sizes[i]);
					}
				}
				m->resizeDocks(shD, shS, orient);
				if (m->layout())
					m->layout()->activate();
				m->resizeDocks(grD, grS, orient);
				if (m->layout())
					m->layout()->activate();
			}
		} /* dock maximums + the center's 0x0 pin restored here */
		if (m->layout())
			m->layout()->activate();
		const int got = sizeOf(first);
		if (std::abs(got - newFirst) > 4) {
			obs_log(LOG_WARNING,
				"divider: drag wanted %d, layout settled at %d (%d docks on the edge; a dock minimum, or Qt refused the trade)",
				newFirst, got, (int)docks.size());
			logAreaDiagnostics(m);
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
	const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
	for (QDockWidget *d : docks) {
		if (d->isVisible() && !d->isFloating() && d->width() > 0 &&
		    m->dockWidgetArea(d) != Qt::NoDockWidgetArea)
			live.push_back(d);
	}

	QList<Spec> specs;
	for (QDockWidget *a : live) {
		for (QDockWidget *b : live) {
			if (a == b || m->dockWidgetArea(a) == m->dockWidgetArea(b))
				continue;
			QRect r = boundaryRect(a->geometry(), b->geometry(), Qt::Horizontal);
			if (r.isValid())
				specs.append({r, a, b, Qt::Horizontal});
			r = boundaryRect(a->geometry(), b->geometry(), Qt::Vertical);
			if (r.isValid())
				specs.append({r, a, b, Qt::Vertical});
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
