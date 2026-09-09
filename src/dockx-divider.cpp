/*
DockX for OBS Studio (by StrmrX) -- drag handles for frozen dock boundaries.
GPL v2, see plugin-main.cpp for the full notice.

Qt's QMainWindow dock areas never trade space with each other directly, only
with the central widget between them. While DockX collapses the main preview
the central widget is pinned to zero size, so the boundary between opposite
dock areas (a left column against a right column) has no mediator: Qt's own
separator there freezes in both directions. This module finds exactly those
boundaries (two visible docked docks from DIFFERENT dock areas sitting edge
to edge), lays a thin invisible handle widget over each, and performs the
drag itself through QMainWindow::resizeDocks -- the official programmatic
equivalent of a separator drag -- so the columns trade space directly.

Defensive by design: handles are plain transparent widgets on top of the
separator gap; they intercept nothing else, never touch Qt layout internals,
and are torn down the moment the preview expands (Qt's native separators work
again then). Separators BETWEEN docks in the same area stay native and are
never covered.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QLayout>
#include <QMainWindow>
#include <QMouseEvent>
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
/* never drag a dock smaller than this (resizeDocks also honors real minimums) */
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
		/* stay invisible no matter what the theme styles plain QWidgets to */
		setStyleSheet("background: transparent;");
		setCursor(o == Qt::Horizontal ? Qt::SplitHCursor : Qt::SplitVCursor);
		setGeometry(r);
		show();
		raise();
	}

protected:
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton || !first || !second) {
			e->ignore();
			return;
		}
		dragging = true;
		pressGlobal = e->globalPosition().toPoint();
		firstSize = sizeOf(first);
		secondSize = sizeOf(second);
		e->accept();
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!dragging || !first || !second)
			return;
		QMainWindow *m = qobject_cast<QMainWindow *>(parentWidget());
		if (!m)
			return;
		const QPoint g = e->globalPosition().toPoint();
		const int delta = orient == Qt::Horizontal ? g.x() - pressGlobal.x() : g.y() - pressGlobal.y();
		int a = firstSize + delta;
		int b = secondSize - delta;
		if (a < MIN_DOCK) {
			b -= MIN_DOCK - a;
			a = MIN_DOCK;
		}
		if (b < MIN_DOCK) {
			a -= MIN_DOCK - b;
			b = MIN_DOCK;
		}
		m->resizeDocks({first.data(), second.data()}, {a, b}, orient);
		if (m->layout())
			m->layout()->activate();
		/* ride along so the cursor stays on the handle */
		const QRect r = boundaryRect(first->geometry(), second->geometry(), orient);
		if (r.isValid())
			setGeometry(r);
		e->accept();
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		if (dragging && first && second) {
			const QPoint g = e->globalPosition().toPoint();
			const int delta = orient == Qt::Horizontal ? g.x() - pressGlobal.x() : g.y() - pressGlobal.y();
			if (sizeOf(first) == firstSize && std::abs(delta) > 8)
				obs_log(LOG_WARNING,
					"divider: drag moved nothing (resizeDocks refused; a dock at its minimum?)");
		}
		dragging = false;
		e->accept();
		QTimer::singleShot(0, [] { rebuild(); });
	}

private:
	int sizeOf(QDockWidget *d) const { return orient == Qt::Horizontal ? d->width() : d->height(); }

	QPoint pressGlobal;
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
