/*
DockX for OBS Studio (by StrmrX) -- collapse the main video preview.
GPL v2, see plugin-main.cpp for the full notice.

OBS's video canvas is the QMainWindow central widget, not a dock: docks can
only ring it, so it always claims a fixed block in the middle of the window.
Hiding it hands the whole window to the docks; a live Program/Preview source
dock (dockx-sourcedocks.cpp) becomes the movable, resizable stand in.
Collapse = hide the widget AND disable the preview display (the switch OBS's
own "Disable Preview" flips). Both are required: a hidden window does NOT
stop its display from rendering, and presenting frames into a hidden window
stalls the graphics pipeline on Windows (the v0.21.0 stutter bug).
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
		obs_log(LOG_WARNING,
			"collapse preview: no central widget found, doing nothing");
		return;
	}
	const bool on = state().previewCollapsed;
	if (c->isHidden() == on)
		return;
	/* the preview display must actually STOP rendering, not just lose its
	   window: presenting frames into a hidden window stalls the graphics
	   pipeline on Windows (v0.21.0 bug: whole-UI stutter). This is the same
	   switch OBS's own "Disable Preview" flips. Studio mode never reaches
	   here collapsed (onStudioModeEnabled expands first) */
	if (on && !obs_frontend_preview_program_mode_active())
		obs_frontend_set_preview_enabled(false);
	c->setVisible(!on);
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
	const auto r = QMessageBox::question(
		parent, "DockX",
		"The main preview is now collapsed, and you have no video dock "
		"yet.\n\nAdd an editable preview so you can still see your scene "
		"and drag your sources around? It is a normal dock: place it, "
		"resize it, or close it like any other. Bring the main preview "
		"back any time from Tools, DockX: Collapse or expand preview.",
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
		obs_log(LOG_INFO,
			"studio mode enabled: expanding the collapsed preview");
		setCollapsed(false);
	}
}

} // namespace preview
} // namespace dockx
