/*
DockX for OBS Studio (by StrmrX) -- the resize blocker hint.
GPL v2, see plugin-main.cpp for the full notice.

Docks can only shrink to the largest minimum size in their row or column.
When a separator drag hits that wall, OBS just stops following the mouse:
the wall is invisible and reads as a bug (Joey 2026-09-09, and the entire
divider saga started as exactly this confusion). This module watches
native separator drags from the outside and, the moment a drag is clearly
being refused, names the culprit: the dock sitting at its minimum gets a
brief amber flash and a small bubble ("X is as small as it can go").

Defensive by design: a pure OBSERVER. The event filter never consumes an
event, never touches the layout, and only ever creates two short-lived
overlay widgets. If it cannot confidently name a blocker it stays silent.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCursor>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>

#include <algorithm>

namespace dockx {
namespace blocker {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* a drag has to clearly fight the wall before we speak up */
static const int TRIGGER_DRAG = 36;
/* the dragged edge counts as frozen if it moved less than this */
static const int FROZEN_TOL = 8;
/* a dock counts as "at its minimum" within this slack */
static const int MIN_SLACK = 6;

/* ---- the two overlay widgets (both auto-delete) ---- */

class FlashOverlay : public QWidget {
public:
	FlashOverlay(QWidget *parent, const QRect &r) : QWidget(parent)
	{
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setAttribute(Qt::WA_NoSystemBackground);
		setGeometry(r);
		show();
		raise();
		QTimer::singleShot(1100, this, &QWidget::deleteLater);
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		p.fillRect(rect(), QColor(255, 179, 0, 26));
		QPen pen(QColor(255, 179, 0, 210), 3);
		p.setPen(pen);
		p.drawRoundedRect(rect().adjusted(2, 2, -2, -2), 4, 4);
	}
};

static void showBubble(QMainWindow *m, const QString &text, const QPoint &globalPos)
{
	QLabel *b = new QLabel(text, m);
	b->setStyleSheet("QLabel { background: #202020; color: #f0f0f0; border: 1px solid #ffb300; "
			 "border-radius: 6px; padding: 6px 10px; }");
	b->adjustSize();
	QPoint p = m->mapFromGlobal(globalPos) + QPoint(14, 18);
	p.setX(std::clamp(p.x(), 4, std::max(4, m->width() - b->width() - 4)));
	p.setY(std::clamp(p.y(), 4, std::max(4, m->height() - b->height() - 4)));
	b->move(p);
	b->show();
	b->raise();
	QTimer::singleShot(2000, b, &QWidget::deleteLater);
}

void flashBlocked(QDockWidget *dock)
{
	QMainWindow *m = mainWindow();
	if (!m || !dock)
		return;
	new FlashOverlay(m, dock->geometry());
	showBubble(m, QString("\"%1\" is as small as it can go").arg(dock->windowTitle()), QCursor::pos());
	obs_log(LOG_INFO, "blocker: '%s' is at its minimum and blocks the drag",
		dock->objectName().toUtf8().constData());
}

/* ---- watching native separator drags ---- */

/* among the docks forming the shrinking column/row, the blocker is the one
   sitting at its minimum with the LARGEST minimum (it sets the floor) */
static QDockWidget *findBlocker(const QList<QDockWidget *> &docks, bool horizontal)
{
	QDockWidget *best = nullptr;
	int bestMin = -1;
	for (QDockWidget *d : docks) {
		const int size = horizontal ? d->width() : d->height();
		const int eff = std::max(horizontal ? d->minimumWidth() : d->minimumHeight(),
					 horizontal ? d->minimumSizeHint().width() : d->minimumSizeHint().height());
		if (size <= eff + MIN_SLACK && eff > bestMin) {
			bestMin = eff;
			best = d;
		}
	}
	return best;
}

class SeparatorWatch : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *o, QEvent *e) override
	{
		QMainWindow *m = qobject_cast<QMainWindow *>(o);
		if (!m)
			return false;
		switch (e->type()) {
		case QEvent::MouseButtonPress: {
			auto *me = static_cast<QMouseEvent *>(e);
			if (me->button() != Qt::LeftButton)
				break;
			/* only a separator press reaches the main window itself
			   with a split cursor set (Qt's separator hover does it) */
			const Qt::CursorShape cs = m->cursor().shape();
			if (cs != Qt::SplitHCursor && cs != Qt::SplitVCursor)
				break;
			active = true;
			shown = false;
			horizontal = cs == Qt::SplitHCursor;
			press = me->pos();
			rects.clear();
			const auto docks = m->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly);
			for (QDockWidget *d : docks)
				if (d->isVisible() && !d->isFloating() && m->dockWidgetArea(d) != Qt::NoDockWidgetArea)
					rects.append({d, d->geometry()});
			break;
		}
		case QEvent::MouseMove: {
			if (!active || shown)
				break;
			auto *me = static_cast<QMouseEvent *>(e);
			if (!(me->buttons() & Qt::LeftButton))
				break;
			const QPoint pos = me->pos();
			const int delta = horizontal ? pos.x() - press.x() : pos.y() - press.y();
			if (std::abs(delta) < TRIGGER_DRAG)
				break;
			evaluate(m, delta, me->globalPosition().toPoint());
			break;
		}
		case QEvent::MouseButtonRelease:
			active = false;
			break;
		default:
			break;
		}
		return false; /* observer only, never consumes */
	}

private:
	void evaluate(QMainWindow *m, int delta, const QPoint &globalPos)
	{
		Q_UNUSED(m); /* kept for signature symmetry; GCC/Clang -Werror flags it */
		/* the shrinking side is the one the mouse moves toward; find the
		   column/row of docks whose press-time near edge lined up with
		   the separator, then check whether that edge actually moved */
		const bool toward = delta > 0; /* toward = right/bottom side shrinks */
		const int pressLine = horizontal ? press.x() : press.y();

		int line = toward ? INT_MAX : INT_MIN;
		for (const auto &r : rects) {
			if (!r.first)
				continue;
			const int nearEdge = horizontal ? (toward ? r.second.left() : r.second.right())
							: (toward ? r.second.top() : r.second.bottom());
			if (toward && nearEdge >= pressLine - 4)
				line = std::min(line, nearEdge);
			else if (!toward && nearEdge <= pressLine + 4)
				line = std::max(line, nearEdge);
		}
		if (line == INT_MAX || line == INT_MIN || std::abs(line - pressLine) > 48)
			return; /* no clean column at the separator; stay silent */

		QList<QDockWidget *> column;
		bool frozen = false;
		for (const auto &r : rects) {
			if (!r.first)
				continue;
			const int nearEdge = horizontal ? (toward ? r.second.left() : r.second.right())
							: (toward ? r.second.top() : r.second.bottom());
			if (std::abs(nearEdge - line) > FROZEN_TOL)
				continue;
			column.append(r.first);
			const QRect now = r.first->geometry();
			const int cur = horizontal ? (toward ? now.left() : now.right())
						   : (toward ? now.top() : now.bottom());
			frozen = frozen || std::abs(cur - nearEdge) < FROZEN_TOL;
		}
		if (column.isEmpty() || !frozen)
			return; /* the layout is following the mouse; all good */

		if (QDockWidget *blockerDock = findBlocker(column, horizontal)) {
			shown = true; /* once per drag */
			flashBlocked(blockerDock);
			Q_UNUSED(globalPos);
		}
	}

	bool active = false;
	bool shown = false;
	bool horizontal = true;
	QPoint press;
	QList<QPair<QPointer<QDockWidget>, QRect>> rects;
};

static SeparatorWatch *g_watch = nullptr;

void start()
{
	QMainWindow *m = mainWindow();
	if (!m || g_watch)
		return;
	g_watch = new SeparatorWatch(m);
	m->installEventFilter(g_watch);
}

void shutdown()
{
	g_watch = nullptr; /* owned by the main window, dies with it */
}

} // namespace blocker
} // namespace dockx
