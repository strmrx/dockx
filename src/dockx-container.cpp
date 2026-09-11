/*
DockX for OBS Studio (by StrmrX) -- second-monitor managed dock container.
One DockX-owned window (dock nesting on) that lives on another screen. Panels
assigned to it are reparented in and can be nested, tabbed and split freely,
exactly like OBS's own main window -- so you finally get a full-height chat
column plus a stacked column beside it, on your second monitor, as ONE tidy
managed surface instead of a pile of floating docks.

Everything the container holds, the way it is arranged, and the screen + size
of the window all restore each launch. The single hard safety rule: on close
(and on OBS exit) every panel is handed back to OBS's main window FIRST, so a
dock can never be stranded off screen or destroyed under OBS.
GPL v2, see plugin-main.cpp for the full notice.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCloseEvent>
#include <QDockWidget>
#include <QGuiApplication>
#include <QMainWindow>
#include <QRect>
#include <QScreen>
#include <QWidget>

#include <algorithm>

namespace dockx {
namespace container {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* the same stable id the rest of DockX uses: object name, else visible title */
static QString keyOf(const QDockWidget *d)
{
	const QString o = d->objectName();
	return o.isEmpty() ? d->windowTitle() : o;
}

/* forward decls for the window's close handler */
static void captureGeometry();
static void releaseAllHome();

static bool g_shuttingDown = false; /* true only while OBS is exiting */

/* the managed window itself: a plain top-level QMainWindow with dock nesting
   turned on and no menu/central content, so assigned docks fill it entirely */
class ContainerWindow : public QMainWindow {
public:
	ContainerWindow() : QMainWindow(nullptr)
	{
		setObjectName("dockx_container_window");
		setWindowTitle("DockX Panels");
		setAttribute(Qt::WA_DeleteOnClose, false); /* we own the lifetime */
		setAttribute(Qt::WA_QuitOnClose, false);    /* closing it never quits OBS */
		setDockNestingEnabled(true);
		setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks |
			       QMainWindow::AnimatedDocks | QMainWindow::GroupedDragging);
		/* a zero-size central widget lets the dock areas own the whole window */
		QWidget *center = new QWidget(this);
		center->setMaximumSize(0, 0);
		setCentralWidget(center);
	}

protected:
	void closeEvent(QCloseEvent *e) override
	{
		if (g_shuttingDown) {
			QMainWindow::closeEvent(e);
			return;
		}
		/* user closed the container: hand the panels back to OBS, forget
		   the assignment, but remember where the window sat so reopening
		   lands in the same place (Joey 2026-09-11: return them home) */
		captureGeometry();
		releaseAllHome();
		State &s = state();
		s.containerOpen = false;
		s.containerDocks.clear();
		s.containerLayout.clear();
		stateSave();
		obs_log(LOG_INFO, "DockX container closed; panels returned to the main window");
		QMainWindow::closeEvent(e);
	}
};

static ContainerWindow *g_win = nullptr;

static QScreen *screenForIndex(int index)
{
	const QList<QScreen *> screens = QGuiApplication::screens();
	if (index >= 0 && index < screens.size())
		return screens[index];
	return QGuiApplication::primaryScreen();
}

/* find a dock by its stable key, wherever it currently lives */
static QDockWidget *findDock(const QString &key)
{
	if (g_win) {
		for (QDockWidget *d : g_win->findChildren<QDockWidget *>())
			if (keyOf(d) == key)
				return d;
	}
	QMainWindow *m = mainWindow();
	if (m) {
		for (QDockWidget *d : m->findChildren<QDockWidget *>())
			if (keyOf(d) == key)
				return d;
	}
	return nullptr;
}

/* create the window (once) and place it on screen. Uses the saved geometry when
   it still lands on a real screen, else sizes a sensible window on the target */
static void ensureWindow(int screenIndex)
{
	if (!g_win) {
		g_win = new ContainerWindow();

		bool placed = false;
		if (!state().containerGeometry.isEmpty())
			placed = g_win->restoreGeometry(state().containerGeometry);
		if (placed && QGuiApplication::screenAt(g_win->frameGeometry().center()) == nullptr)
			placed = false; /* saved on a monitor that is no longer attached */

		if (!placed) {
			QScreen *scr = screenForIndex(screenIndex);
			if (scr) {
				const QRect a = scr->availableGeometry();
				const int w = a.width() * 2 / 3;
				const int h = a.height() * 4 / 5;
				g_win->resize(w, h);
				g_win->move(a.left() + (a.width() - w) / 2, a.top() + (a.height() - h) / 2);
			}
		}
	}
	/* always end up visible: the window may already exist but hidden after a
	   user close, and a fresh assign should bring it back onto the screen */
	g_win->show();
	g_win->raise();
}

/* remember the window's position/size + inner arrangement into state */
static void captureGeometry()
{
	if (!g_win)
		return;
	state().containerGeometry = g_win->saveGeometry();
	state().containerLayout = g_win->saveState();
	QScreen *scr = QGuiApplication::screenAt(g_win->frameGeometry().center());
	if (scr)
		state().containerScreen = QGuiApplication::screens().indexOf(scr);
}

/* hand every panel in the container back to the OBS main window (never lost) */
static void releaseAllHome()
{
	QMainWindow *m = mainWindow();
	if (!g_win || !m)
		return;
	int moved = 0;
	for (QDockWidget *d : g_win->findChildren<QDockWidget *>()) {
		m->addDockWidget(Qt::RightDockWidgetArea, d);
		d->setFloating(false);
		d->show();
		moved++;
	}
	if (moved)
		obs_log(LOG_INFO, "DockX container returned %d panel(s) to the main window", moved);
}

bool isOpen()
{
	return g_win != nullptr && g_win->isVisible();
}

void openOn(int screenIndex)
{
	ensureWindow(screenIndex);
	state().containerOpen = true;
	captureGeometry();
	stateSave();
}

void close()
{
	if (g_win)
		g_win->close(); /* runs closeEvent: releases panels + saves */
}

void assign(const QStringList &dockKeys, int screenIndex)
{
	if (dockKeys.isEmpty())
		return;
	ensureWindow(screenIndex);
	state().containerOpen = true;

	int added = 0;
	for (const QString &key : dockKeys) {
		QDockWidget *d = findDock(key);
		if (!d)
			continue;
		if (d->parentWidget() == g_win)
			continue; /* already in the container */
		g_win->addDockWidget(Qt::LeftDockWidgetArea, d);
		d->setFloating(false);
		d->show();
		if (!state().containerDocks.contains(key))
			state().containerDocks << key;
		added++;
	}
	g_win->raise();
	g_win->activateWindow();
	captureGeometry();
	stateSave();
	obs_log(LOG_INFO, "DockX container took in %d panel(s)", added);
}

void sendBack(const QStringList &dockKeys)
{
	QMainWindow *m = mainWindow();
	if (!g_win || !m)
		return;
	int moved = 0;
	for (const QString &key : dockKeys) {
		QDockWidget *d = nullptr;
		for (QDockWidget *c : g_win->findChildren<QDockWidget *>())
			if (keyOf(c) == key) {
				d = c;
				break;
			}
		if (!d)
			continue;
		m->addDockWidget(Qt::RightDockWidgetArea, d);
		d->setFloating(false);
		d->show();
		state().containerDocks.removeAll(key);
		moved++;
	}
	captureGeometry();
	stateSave();
	if (moved)
		obs_log(LOG_INFO, "DockX container sent %d panel(s) back to the main window", moved);
}

QList<panels::DockInfo> contained()
{
	QList<panels::DockInfo> out;
	if (!g_win)
		return out;
	for (QDockWidget *d : g_win->findChildren<QDockWidget *>()) {
		if (d->windowTitle().isEmpty())
			continue;
		out.append({keyOf(d), d->windowTitle()});
	}
	std::sort(out.begin(), out.end(), [](const panels::DockInfo &a, const panels::DockInfo &b) {
		return a.title.compare(b.title, Qt::CaseInsensitive) < 0;
	});
	return out;
}

void restoreFromState()
{
	State &s = state();
	if (!s.containerOpen)
		return;
	QMainWindow *m = mainWindow();
	if (!m)
		return;

	ensureWindow(s.containerScreen);

	/* pull the assigned panels in; a key that does not resolve (source gone,
	   or it belongs to a different scene collection) is simply skipped */
	for (const QString &key : s.containerDocks) {
		QDockWidget *d = nullptr;
		for (QDockWidget *c : m->findChildren<QDockWidget *>())
			if (keyOf(c) == key) {
				d = c;
				break;
			}
		if (!d)
			continue;
		g_win->addDockWidget(Qt::LeftDockWidgetArea, d);
		d->setFloating(false);
		d->show();
	}
	/* now restore the nesting arrangement (matched by object name) */
	if (!s.containerLayout.isEmpty())
		g_win->restoreState(s.containerLayout);
	g_win->show();
	g_win->raise();
	obs_log(LOG_INFO, "DockX container reopened on monitor %d", s.containerScreen + 1);
}

void shutdown()
{
	/* OBS is exiting: capture the arrangement, then hand every panel back to
	   the main window BEFORE our window dies, so OBS never destroys a dock it
	   still holds a pointer to. containerOpen/containerDocks stay as-is so the
	   container reopens exactly like this next launch. */
	g_shuttingDown = true;
	if (!g_win)
		return;
	captureGeometry();
	releaseAllHome();
	delete g_win;
	g_win = nullptr;
}

} // namespace container
} // namespace dockx
