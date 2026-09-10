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

The drag itself (v0.46.11, SIXTH design -- the first five failed on the rig):
- While the mouse is down NOTHING in the layout moves. The handle paints
  itself as a ghost bar and follows the mouse. (v0.45.1 forced a relayout
  per mouse move; every video dock's obs_display resized every few ms, the
  UI glitched, and the layout stormed. Never again.)
- On release the trade is applied ONCE by SAVESTATE SURGERY: snapshot the
  layout (QMainWindow::saveState), patch the stored size integers of every
  area/item on either side of the boundary, restoreState with the center's
  0x0 pin briefly lifted (see the blob namespace + TradeFlexScope below).
  Why: every live-layout design failed on the rig. min=max pinning snapped
  back (0.45.1); direct cross-area resizeDocks is silently ignored
  (0.45.0); center-mediated resizeDocks gets re-balanced away because
  top/bottom band widths are DERIVED and the right area reshuffles
  internally (0.46.3-0.46.9, five identical "settled at 304" logs);
  synthetic native-separator drags find nothing to grab because the pinned
  0x0 center leaves its separators zero pixels long (0.46.10). The
  save/restore path is the one mechanism PROVEN to reproduce this exact
  cross-band layout: it does so on every OBS restart.

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
#include <QSet>
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

/* ---- saveState blob surgery (divider design v6) ----

   The ONE path proven on Joey's rig to reproduce cross-band boundaries
   faithfully is QMainWindow::saveState/restoreState: his layout (with all
   its derived-width bands) survives every OBS restart bit-perfect. The
   snapshot literally stores every area's size and every item's extent as
   plain integers, so a boundary drag becomes: saveState -> patch the
   integers on both sides of the boundary -> restoreState.

   Format (verified against Qt 6.8 qdockarealayout.cpp/qmainwindowlayout.cpp,
   copies in the session scratchpad; QDataStream big-endian, Qt_5_0):
     qint32 0xff, qint32 version          (QMainWindow::saveState header)
     uchar  0xfd                          (DockWidgetStateMarker; dock
                                           section is FIRST in the layout
                                           state, before floating tab groups
                                           and toolbars)
     qint32 areaCount
     per area: qint32 areaPos, qint32 w, qint32 h, then info:
       uchar 0xfa(tab)+qint32 index | 0xfc(sequence)
       uchar orientation (1=H, 2=V), qint32 itemCount
       per item:
         0xfb widget: QString objectName, uchar flags,
              4x qint32 (floating: x,y,w,h; docked: pos,size,min,max)
         0xfc subinfo: 4x qint32 (pos,size,min,max), recursive info
     QSize centralWidgetRect, 4x qint32 corners
   Everything after the dock section is left byte-identical. */

namespace blob {

struct Cursor {
	const uchar *p;
	int n;
	int off = 0;
	bool ok = true;

	uchar u8()
	{
		if (off + 1 > n) {
			ok = false;
			return 0;
		}
		return p[off++];
	}
	qint32 i32()
	{
		if (off + 4 > n) {
			ok = false;
			return 0;
		}
		qint32 v = (qint32)((quint32)p[off] << 24 | (quint32)p[off + 1] << 16 | (quint32)p[off + 2] << 8 |
				    (quint32)p[off + 3]);
		off += 4;
		return v;
	}
	QString str()
	{
		const quint32 len = (quint32)i32();
		if (len == 0xFFFFFFFFu)
			return QString(); /* null string */
		if (!ok || off + (int)len > n || (len % 2) != 0) {
			ok = false;
			return QString();
		}
		QString s;
		s.reserve((int)len / 2);
		for (quint32 i = 0; i < len; i += 2)
			s.append(QChar((ushort)((ushort)p[off + i] << 8 | (ushort)p[off + i + 1])));
		off += (int)len;
		return s;
	}
};

/* one patchable size int: its byte offset, and whether it is an extent
   along the horizontal axis */
struct ItemRef {
	int sizeOff = 0;
	bool horiz = false;
	QStringList names; /* every dock objectName in this item's subtree */
};

struct AreaRef {
	int wOff = 0, hOff = 0;
	QStringList names;
	QList<ItemRef> topItems; /* the area's DIRECT children only */
};

/* parse one QDockAreaLayoutInfo; fills names; when topItems is non-null the
   info's direct items are recorded there (top level of an area) */
static void parseInfo(Cursor &c, QStringList &names, QList<ItemRef> *topItems)
{
	const uchar marker = c.u8();
	if (marker == 0xfa) {
		c.i32(); /* current tab index */
	} else if (marker != 0xfc) {
		c.ok = false;
		return;
	}
	const uchar o = c.u8(); /* 1 = Horizontal, 2 = Vertical */
	const qint32 cnt = c.i32();
	if (!c.ok || cnt < 0 || cnt > 512) {
		c.ok = false;
		return;
	}
	for (qint32 i = 0; i < cnt && c.ok; i++) {
		const uchar im = c.u8();
		ItemRef ref;
		ref.horiz = o == 1;
		if (im == 0xfb) { /* widget (or placeholder) */
			const QString name = c.str();
			const uchar flags = c.u8();
			if (flags & 2) { /* floating: x,y,w,h */
				c.i32();
				c.i32();
				c.i32();
				c.i32();
			} else {
				c.i32(); /* pos */
				ref.sizeOff = c.off;
				c.i32(); /* size */
				c.i32(); /* min */
				c.i32(); /* max */
			}
			ref.names << name;
			names << name;
		} else if (im == 0xfc) { /* nested info */
			c.i32();         /* pos */
			ref.sizeOff = c.off;
			c.i32(); /* size */
			c.i32(); /* min */
			c.i32(); /* max */
			QStringList sub;
			parseInfo(c, sub, nullptr);
			ref.names = sub;
			names << sub;
		} else {
			c.ok = false;
			return;
		}
		if (topItems && ref.sizeOff)
			topItems->append(ref);
	}
}

/* parse the dock section of a QMainWindow::saveState blob */
static bool parse(const QByteArray &state, QList<AreaRef> &areas)
{
	Cursor c{(const uchar *)state.constData(), (int)state.size()};
	if (c.i32() != 0xff)
		return false; /* VersionMarker */
	c.i32();              /* version */
	if (c.u8() != 0xfd)
		return false; /* DockWidgetStateMarker */
	const qint32 cnt = c.i32();
	if (!c.ok || cnt < 0 || cnt > 4)
		return false;
	for (qint32 i = 0; i < cnt && c.ok; i++) {
		c.i32(); /* area position index */
		AreaRef a;
		a.wOff = c.off;
		c.i32(); /* w */
		a.hOff = c.off;
		c.i32(); /* h */
		parseInfo(c, a.names, &a.topItems);
		areas.append(a);
	}
	return c.ok;
}

static void writeI32(QByteArray &state, int off, qint32 v)
{
	uchar *p = (uchar *)state.data() + off;
	p[0] = (uchar)((quint32)v >> 24);
	p[1] = (uchar)((quint32)v >> 16);
	p[2] = (uchar)((quint32)v >> 8);
	p[3] = (uchar)v;
}

static qint32 readI32(const QByteArray &state, int off)
{
	const uchar *p = (const uchar *)state.constData() + off;
	return (qint32)((quint32)p[0] << 24 | (quint32)p[1] << 16 | (quint32)p[2] << 8 | (quint32)p[3]);
}

} // namespace blob

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
		const int delta = newFirst - firstSize;
		const bool horiz = orient == Qt::Horizontal;

		/* classify every dock by which side of the dragged boundary its
		   edge sits on (whole-edge semantics: a visual column can span
		   multiple Qt areas, so the patch must cover every aligned dock) */
		const int edgeA = horiz ? first->geometry().right() : first->geometry().bottom();
		const int edgeB = horiz ? second->geometry().left() : second->geometry().top();
		QSet<QString> sideA, sideB; /* dock objectNames before/after the line */
		const auto all = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
		for (QDockWidget *d : all) {
			if (!d->isVisible() || d->isFloating() || m->dockWidgetArea(d) == Qt::NoDockWidgetArea)
				continue;
			const QRect g = d->geometry();
			if (std::abs((horiz ? g.right() : g.bottom()) - edgeA) <= MAX_GAP)
				sideA.insert(d->objectName());
			else if (std::abs((horiz ? g.left() : g.top()) - edgeB) <= MAX_GAP)
				sideB.insert(d->objectName());
		}

		/* snapshot -> patch the stored sizes -> restore. save/restore is
		   the one mechanism proven (by every OBS restart) to reproduce
		   this layout's cross-band boundaries faithfully; patching its
		   integers sidesteps the live layout negotiation that swallowed
		   every resizeDocks/separator attempt (designs 1-5). */
		QByteArray state = m->saveState();
		QList<blob::AreaRef> areas;
		if (!blob::parse(state, areas)) {
			obs_log(LOG_WARNING, "divider: could not parse the layout state; drag ignored");
			return;
		}
		auto side = [&](const QStringList &names) {
			bool a = false, b = false;
			for (const QString &n : names) {
				a = a || sideA.contains(n);
				b = b || sideB.contains(n);
			}
			return a == b ? 0 : (a ? 1 : 2); /* 0 = neither or both (skip) */
		};
		int patched = 0;
		for (const blob::AreaRef &ar : areas) {
			const int areaSide = side(ar.names);
			const int off = horiz ? ar.wOff : ar.hOff;
			if (areaSide == 1) {
				blob::writeI32(state, off, blob::readI32(state, off) + delta);
				patched++;
			} else if (areaSide == 2) {
				blob::writeI32(state, off, blob::readI32(state, off) - delta);
				patched++;
			}
			/* the area's DIRECT items partition it along its own axis;
			   only same-axis items carry the boundary */
			for (const blob::ItemRef &it : ar.topItems) {
				if (it.horiz != horiz)
					continue;
				const int s = side(it.names);
				if (s == 0)
					continue;
				blob::writeI32(state, it.sizeOff,
					       blob::readI32(state, it.sizeOff) + (s == 1 ? delta : -delta));
				patched++;
			}
		}
		if (!patched) {
			obs_log(LOG_WARNING, "divider: found nothing to patch for this boundary; drag ignored");
			return;
		}
		{
			TradeFlexScope flex(m, first, second);
			/* restore with the center flexible, exactly like OBS startup
			   (state restores BEFORE DockX pins the center 0x0 -- the
			   condition under which these sizes provably round-trip) */
			m->restoreState(state);
			if (m->layout())
				m->layout()->activate();
		} /* dock maximums + the center's 0x0 pin restored here */
		if (m->layout())
			m->layout()->activate();
		const int got = sizeOf(first);
		if (std::abs(got - newFirst) > 4) {
			obs_log(LOG_WARNING,
				"divider: drag wanted %d, layout settled at %d (state surgery, %d ints patched; a dock minimum, or the layout re-derived)",
				newFirst, got, patched);
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
