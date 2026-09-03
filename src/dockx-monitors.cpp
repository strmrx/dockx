/*
DockX for OBS Studio (by StrmrX) -- multi monitor dock manager.
Send docks to any screen (tiled), keep saved layouts monitor safe, and bring
stranded docks home when a monitor is unplugged so one is never lost.
GPL v2, see plugin-main.cpp for the full notice.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QDockWidget>
#include <QGuiApplication>
#include <QMainWindow>
#include <QPoint>
#include <QRect>
#include <QScreen>
#include <QTimer>

#include <cmath>

namespace dockx {
namespace monitors {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* same stable id the rest of DockX uses: object name, else the visible title */
static QString keyOf(const QDockWidget *d)
{
	const QString o = d->objectName();
	return o.isEmpty() ? d->windowTitle() : o;
}

static QDockWidget *findByKey(QMainWindow *m, const QString &key)
{
	const QList<QDockWidget *> docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks)
		if (keyOf(d) == key)
			return d;
	return nullptr;
}

/* a floating dock is stranded when the grab point on its title bar sits on no
   screen at all: the user cannot reach it to drag it back */
static bool stranded(QDockWidget *d)
{
	if (!d->isFloating() || !d->isVisible())
		return false;
	const QRect fr = d->frameGeometry();
	const QPoint handle(fr.center().x(), fr.top() + 4);
	return QGuiApplication::screenAt(handle) == nullptr;
}

QList<ScreenInfo> listScreens()
{
	QList<ScreenInfo> out;
	const QList<QScreen *> screens = QGuiApplication::screens();
	QScreen *primary = QGuiApplication::primaryScreen();
	for (int i = 0; i < screens.size(); i++) {
		QScreen *s = screens[i];
		const QRect g = s->geometry();
		QString label = QString("Monitor %1 \xC2\xB7 %2\xC3\x97%3").arg(i + 1).arg(g.width()).arg(g.height());
		if (s == primary)
			label += " (main)";
		out.append({i, label, g});
	}
	return out;
}

int sendDocksToScreen(const QStringList &keys, int screenIndex)
{
	QMainWindow *m = mainWindow();
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (!m || screenIndex < 0 || screenIndex >= screens.size())
		return 0;
	const QRect area = screens[screenIndex]->availableGeometry();

	QList<QDockWidget *> docks;
	for (const QString &k : keys) {
		QDockWidget *d = findByKey(m, k);
		if (d && !d->windowTitle().isEmpty())
			docks.append(d);
	}
	const int n = docks.size();
	if (n == 0)
		return 0;

	/* tile into as square a grid as the count allows */
	const int cols = (int)std::ceil(std::sqrt((double)n));
	const int rows = (int)std::ceil((double)n / cols);
	const int margin = 8;
	const int cellW = (area.width() - margin * (cols + 1)) / cols;
	const int cellH = (area.height() - margin * (rows + 1)) / rows;
	if (cellW < 120 || cellH < 90)
		return 0; /* screen too small to tile this many; do nothing */

	for (int i = 0; i < n; i++) {
		const int r = i / cols;
		const int c = i % cols;
		const int x = area.left() + margin + c * (cellW + margin);
		const int y = area.top() + margin + r * (cellH + margin);
		QDockWidget *d = docks[i];
		if (!d->isFloating())
			d->setFloating(true);
		d->resize(cellW, cellH);
		d->move(x, y);
		d->show();
		d->raise();
	}
	obs_log(LOG_INFO, "DockX sent %d dock(s) to monitor %d", n, screenIndex + 1);
	return n;
}

int rescueStrayDocks()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return 0;
	/* home = the screen OBS itself is on, else the primary */
	QScreen *home = QGuiApplication::screenAt(m->frameGeometry().center());
	if (!home)
		home = QGuiApplication::primaryScreen();
	if (!home)
		return 0;
	const QRect area = home->availableGeometry();

	int moved = 0;
	const QList<QDockWidget *> docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks) {
		if (d->windowTitle().isEmpty())
			continue;
		if (!stranded(d))
			continue;
		/* cascade recovered docks so several do not stack on one spot */
		const int off = moved * 32;
		int w = qMin(d->width(), area.width() - 80);
		int h = qMin(d->height(), area.height() - 80);
		if (w < 160)
			w = 160;
		if (h < 120)
			h = 120;
		int x = area.left() + 40 + off;
		int y = area.top() + 40 + off;
		if (x + w > area.right())
			x = area.left() + 40;
		if (y + h > area.bottom())
			y = area.top() + 40;
		d->resize(w, h);
		d->move(x, y);
		d->show();
		d->raise();
		moved++;
	}
	if (moved)
		obs_log(LOG_INFO, "DockX rescued %d off screen dock(s)", moved);
	return moved;
}

void validateVisible()
{
	/* after a layout is restored, reflow anything that landed off every
	   screen (e.g. a layout saved for more monitors than are attached now) */
	rescueStrayDocks();
}

static bool watchInstalled = false;

void installWatch()
{
	if (watchInstalled)
		return;
	QApplication *app = qApp;
	if (!app)
		return;
	QObject::connect(app, &QGuiApplication::screenRemoved, app, [](QScreen *) {
		if (!state().autoRescue)
			return;
		QMainWindow *m = mainWindow();
		if (!m)
			return;
		/* let Windows settle window positions after the unplug first */
		QTimer::singleShot(400, m, []() { rescueStrayDocks(); });
	});
	watchInstalled = true;
	obs_log(LOG_INFO, "DockX monitor watch installed");
}

} // namespace monitors
} // namespace dockx
