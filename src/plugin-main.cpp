/*
DockX for OBS Studio (by StrmrX)
Unlocks flexible dock layouts: turns on Qt dock nesting for the OBS main
window, so docks can sit in side-by-side columns instead of rows only.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QMessageBox>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static bool enable_nesting(void)
{
	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main)
		return false;
	main->setDockNestingEnabled(true);
	obs_log(LOG_INFO, "dock nesting enabled on the OBS main window");
	return true;
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	/* enable again after full load: covers the case where the main window
	   was not ready at module load, and wins if anything reset the flag
	   while the saved layout was being restored */
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		enable_nesting();
}

static void tools_menu_clicked(void *)
{
	QMainWindow *main = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	bool on = main && main->isDockNestingEnabled();
	if (on) {
		QMessageBox::information(main, "DockX",
			"DockX is active. Flexible dock layouts are ON.\n\n"
			"Drag a dock onto the left or right EDGE of another dock to "
			"place them side by side and build columns.\n\n"
			"Tip: turn on Docks > Full-Height Docks so side columns can "
			"run the full height of the window.");
	} else {
		QMessageBox::warning(main, "DockX",
			"DockX is installed but could not switch flexible layouts on. "
			"Check the OBS log (Help > Log Files) for lines mentioning dockx.");
	}
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "DockX loaded (version %s)", PLUGIN_VERSION);
	/* enable as early as possible so a saved nested layout restores
	   correctly on startup */
	if (!enable_nesting())
		obs_log(LOG_INFO, "main window not ready yet, will retry when OBS finishes loading");
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_frontend_add_tools_menu_item("DockX", tools_menu_clicked, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "DockX unloaded");
}
