/*
DockX for OBS Studio (by StrmrX) -- starter layout templates.
GPL v2, see plugin-main.cpp for the full notice.

New users don't know how to build a good multi-column OBS layout by hand, so
DockX ships a few one-click starting points. A template is not a saved blob (it
can't be -- every user's docks differ); it arranges the docks THIS OBS actually
has, right now, via the QMainWindow dock API:
  - the DockX Preview (added if none exists) is the editable video surface,
  - OBS's core panels (Scenes / Sources / Mixer / Transitions / Controls) are
    tiled or tabbed per template,
  - the user's chat dock is detected by name and placed; if there is none, a
    labeled placeholder shows where to drop one.

OBS's fixed center preview is collapsed (flexible mode) so the docks own the
window. Every apply snapshots the current layout into state().undoState first,
so panels::undoLayout() reverts it -- a template is always a safe starting point,
never a trap, even if a power user fires one at a hand-built layout.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>

namespace dockx {
namespace templates {

namespace {

QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

QDockWidget *byName(QMainWindow *m, const QString &objName)
{
	const auto docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks)
		if (d->objectName() == objName)
			return d;
	return nullptr;
}

/* a user chat dock, matched by object name or title (Twitch / YouTube / Kick
   chat browser docks all contain "chat"); our own placeholder is excluded */
QDockWidget *findChat(QMainWindow *m)
{
	QDockWidget *any = nullptr;
	const auto docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks) {
		if (d->objectName() == "dockx_chat_slot")
			continue;
		const bool isChat =
			d->objectName().contains("chat", Qt::CaseInsensitive) ||
			d->windowTitle().contains("chat", Qt::CaseInsensitive);
		if (!isChat)
			continue;
		if (d->isVisible())
			return d; /* prefer an open one */
		if (!any)
			any = d;
	}
	return any;
}

/* OBS's core panels, in a sensible stacking order; missing ones are skipped */
QList<QDockWidget *> findPanels(QMainWindow *m)
{
	QList<QDockWidget *> out;
	const char *names[] = {"scenesDock", "sourcesDock", "mixerDock",
			       "transitionsDock", "controlsDock"};
	for (const char *n : names) {
		QDockWidget *d = byName(m, n);
		if (d)
			out.push_back(d);
	}
	return out;
}

/* the DockX Preview dock; adds one if none exists yet */
QDockWidget *ensurePreview(QMainWindow *m)
{
	QList<int> ids = editpreview::dockIds();
	if (ids.isEmpty()) {
		editpreview::addDock();
		ids = editpreview::dockIds();
	}
	if (ids.isEmpty())
		return nullptr;
	return byName(m, QString("dockx_edit_%1").arg(ids.first()));
}

/* the "add your chat here" placeholder: created once, reused, so re-applying a
   template never stacks up copies. Not registered with OBS (session only). */
QDockWidget *ensurePlaceholder(QMainWindow *m)
{
	QDockWidget *d = byName(m, "dockx_chat_slot");
	if (d)
		return d;
	d = new QDockWidget("Add your chat here", m);
	d->setObjectName("dockx_chat_slot");
	QLabel *lbl = new QLabel(
		"Your chat goes in this column.\n\nAdd your chat as a browser dock "
		"(your Twitch, YouTube or Kick chat, or OBS's Docks menu), then apply "
		"this template again to slot it in.",
		d);
	lbl->setWordWrap(true);
	lbl->setAlignment(Qt::AlignCenter);
	lbl->setMargin(16);
	d->setWidget(lbl);
	return d;
}

void removePlaceholder(QMainWindow *m)
{
	QDockWidget *d = byName(m, "dockx_chat_slot");
	if (d) {
		m->removeDockWidget(d);
		d->deleteLater();
	}
}

void placed(QDockWidget *d)
{
	if (!d)
		return;
	d->setFloating(false);
	d->setVisible(true);
}

/* ---- the three starter arrangements ---- */

/* tall chat column on the left; editable preview top-right; core panels tiled
   in a row beneath it */
void verticalChatFocus(QMainWindow *m, QDockWidget *preview, QDockWidget *chat,
		       const QList<QDockWidget *> &panels)
{
	const int W = m->width() > 0 ? m->width() : 1280;
	const int H = m->height() > 0 ? m->height() : 720;

	m->addDockWidget(Qt::RightDockWidgetArea, preview);
	placed(preview);

	QDockWidget *rowAnchor = nullptr;
	for (QDockWidget *p : panels) {
		if (!rowAnchor)
			m->splitDockWidget(preview, p, Qt::Vertical);
		else
			m->splitDockWidget(rowAnchor, p, Qt::Horizontal);
		rowAnchor = p;
		placed(p);
	}

	m->addDockWidget(Qt::LeftDockWidgetArea, chat);
	placed(chat);

	m->resizeDocks({chat, preview}, {(int)(W * 0.24), (int)(W * 0.76)},
		       Qt::Horizontal);
	if (rowAnchor)
		m->resizeDocks({preview, rowAnchor}, {(int)(H * 0.60), (int)(H * 0.40)},
			       Qt::Vertical);
}

/* big editable preview across the top; chat + panels tiled along the bottom */
void widePreview(QMainWindow *m, QDockWidget *preview, QDockWidget *chat,
		 const QList<QDockWidget *> &panels)
{
	const int H = m->height() > 0 ? m->height() : 720;

	m->addDockWidget(Qt::TopDockWidgetArea, preview);
	placed(preview);

	QList<QDockWidget *> row;
	row << chat;
	row += panels;

	QDockWidget *anchor = nullptr;
	for (QDockWidget *d : row) {
		if (!anchor)
			m->addDockWidget(Qt::BottomDockWidgetArea, d);
		else
			m->splitDockWidget(anchor, d, Qt::Horizontal);
		anchor = d;
		placed(d);
	}
	if (anchor)
		m->resizeDocks({preview, anchor}, {(int)(H * 0.62), (int)(H * 0.38)},
			       Qt::Vertical);
}

/* everything tight for a small screen: preview on the left, chat + panels tabbed
   into one stack on the right */
void compact(QMainWindow *m, QDockWidget *preview, QDockWidget *chat,
	     const QList<QDockWidget *> &panels)
{
	const int W = m->width() > 0 ? m->width() : 1280;

	m->addDockWidget(Qt::LeftDockWidgetArea, preview);
	placed(preview);

	QList<QDockWidget *> stack = panels;
	stack << chat;

	QDockWidget *first = nullptr;
	for (QDockWidget *d : stack) {
		if (!first)
			m->addDockWidget(Qt::RightDockWidgetArea, d);
		else
			m->tabifyDockWidget(first, d);
		if (!first)
			first = d;
		placed(d);
	}
	if (first)
		m->resizeDocks({preview, first}, {(int)(W * 0.58), (int)(W * 0.42)},
			       Qt::Horizontal);
}

} // namespace

QList<Info> list()
{
	QList<Info> out;
	out.push_back({"vertical_chat",
		       "Vertical Chat Focus",
		       "A tall chat column on the left, your editable DockX Preview "
		       "top right, and the Scenes, Sources and Mixer panels tiled "
		       "beneath it."});
	out.push_back({"wide_preview",
		       "Wide Preview",
		       "A big editable DockX Preview across the top, with chat and "
		       "your panels tiled along the bottom."});
	out.push_back({"compact",
		       "Compact",
		       "Everything tight for a small screen: the preview on the left, "
		       "chat and panels tabbed together on the right."});
	return out;
}

bool apply(const QString &id, QWidget *)
{
	QMainWindow *m = mainWindow();
	if (!m)
		return false;

	/* snapshot first so panels::undoLayout() puts it all back */
	state().undoState = m->saveState();

	m->setDockNestingEnabled(true);

	/* flexible mode: hide OBS's fixed center preview so the docks own the window
	   and the DockX Preview is the video surface */
	preview::setCollapsed(true);

	QDockWidget *prev = ensurePreview(m);
	if (!prev) {
		obs_log(LOG_WARNING, "template %s: no DockX Preview to place",
			id.toUtf8().constData());
		return false;
	}

	QDockWidget *chat = findChat(m);
	if (chat)
		removePlaceholder(m);
	else
		chat = ensurePlaceholder(m); /* labeled gap where chat belongs */

	const QList<QDockWidget *> panels = findPanels(m);

	if (id == "vertical_chat")
		verticalChatFocus(m, prev, chat, panels);
	else if (id == "wide_preview")
		widePreview(m, prev, chat, panels);
	else if (id == "compact")
		compact(m, prev, chat, panels);
	else
		return false;

	locks::applyHardLock();      /* re-docking can re-show docks; reassert a freeze */
	monitors::validateVisible(); /* never leave a dock off screen */
	obs_log(LOG_INFO, "applied layout template \"%s\"", id.toUtf8().constData());
	return true;
}

} // namespace templates
} // namespace dockx
