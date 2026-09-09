/*
DockX for OBS Studio (by StrmrX) -- collapse the main video preview.
GPL v2, see plugin-main.cpp for the full notice.

OBS's video canvas is the QMainWindow central widget, not a dock: docks can
only ring it, so it always claims a fixed block in the middle of the window.
Collapsing it hands the whole window to the docks; a live Program/Preview
source dock (dockx-sourcedocks.cpp) becomes the movable, resizable stand in.
Collapse = pin the widget to ZERO SIZE (visible, min and max forced to 0x0)
AND disable the preview display (the switch OBS's own "Disable Preview"
flips). Disabling is required: a zero sized or hidden window does NOT stop
its display from rendering, and presenting frames into one stalls the
graphics pipeline on Windows (the v0.21.0 stutter bug).
Zero size instead of hidden (v0.45.0): a HIDDEN central widget leaves the
QMainWindow layout with no center item at all, which froze the separator
between opposite dock areas AND let the layout drift ("settling") -- dock
areas only ever negotiate space through the center. Pinned at 0x0 the center
stays in the layout as a fully constrained item, so the negotiation is
stable; dockx-divider.cpp then bridges the still-frozen between-areas
separators with its own drag handles.
Stream/record/sources keep running throughout.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QMessageBox>
#include <QWidget>

namespace dockx {
namespace preview {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

bool collapsed()
{
	return state().previewCollapsed;
}

void apply()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	QWidget *c = m->centralWidget();
	if (!c) {
		obs_log(LOG_WARNING, "collapse preview: no central widget found, doing nothing");
		return;
	}
	const bool on = state().previewCollapsed;
	/* tracked here, not read off the widget: collapsed no longer hides it */
	static bool constrained = false;
	static QSize savedMin, savedMax;
	if (constrained == on) {
		divider::setActive(on);
		return;
	}
	/* the preview display must actually STOP rendering, not just shrink:
	   presenting frames into a zero sized or hidden window stalls the
	   graphics pipeline on Windows (v0.21.0 bug: whole-UI stutter). This is
	   the same switch OBS's own "Disable Preview" flips. Studio mode never
	   reaches here collapsed (onStudioModeEnabled expands first) */
	if (on && !obs_frontend_preview_program_mode_active())
		obs_frontend_set_preview_enabled(false);
	if (on) {
		savedMin = c->minimumSize();
		savedMax = c->maximumSize();
		c->setMinimumSize(0, 0);
		c->setMaximumSize(0, 0);
	} else {
		c->setMinimumSize(savedMin);
		c->setMaximumSize(savedMax.isValid() ? savedMax : QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
	}
	/* stays VISIBLE either way: a hidden center drops out of the layout and
	   the dock areas lose their space mediator (frozen separators + drift) */
	c->setVisible(true);
	constrained = on;
	divider::setActive(on);
	if (!on && !obs_frontend_preview_program_mode_active())
		obs_frontend_set_preview_enabled(true);
	/* collapsed with no visible video dock = a video-less OBS; surface ONE
	   video dock -- don't pile them up. Prefer the editable preview (the
	   movable, editable stand-in that shows video off-stream); only if there
	   is none fall back to any Program/Preview dock. Covers boot, where no
	   prompt is possible. */
	if (on) {
		if (!editpreview::showDocks())
			sourcedocks::showVideoDocks();
	}
	obs_log(LOG_INFO, "main preview %s", on ? "collapsed" : "expanded");
	/* keep every DockX Preview's "Show/Hide OBS preview" button in sync */
	editpreview::refreshAll();
}

void setCollapsed(bool on)
{
	if (state().previewCollapsed == on)
		return;
	state().previewCollapsed = on;
	apply();
	stateSave();
}

void offerVideoDock(QWidget *parent)
{
	/* a video dock the user cannot see may as well not exist: if any editable
	   Preview or Program/Preview dock is registered, surface it (OBS keeps a
	   closed dock closed forever) instead of leaving a video-less window */
	if (editpreview::showDocks() || sourcedocks::showVideoDocks())
		return;
	const auto r = QMessageBox::question(parent, "DockX",
					     "OBS's built-in preview is now hidden, and you have no DockX "
					     "Preview yet.\n\nAdd a DockX Preview? It is a movable, editable "
					     "window of your scene: drag your sources right in it, place or "
					     "resize it like any dock, and its \"Show OBS preview\" button "
					     "brings OBS's built-in preview back any time.",
					     QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
	if (r == QMessageBox::Yes)
		editpreview::addDock();
}

void toggleWithPrompt(QWidget *parent)
{
	const bool on = !state().previewCollapsed;
	setCollapsed(on);
	if (on)
		offerVideoDock(parent);
}

void onStudioModeEnabled()
{
	/* studio mode edits happen on the main canvas; never leave someone in
	   studio mode staring at a hidden editor */
	if (state().previewCollapsed) {
		obs_log(LOG_INFO, "studio mode enabled: expanding the collapsed preview");
		setCollapsed(false);
	}
}

} // namespace preview
} // namespace dockx
