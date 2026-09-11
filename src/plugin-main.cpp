/*
DockX for OBS Studio (by StrmrX)
Makes OBS's dock system yours: flexible column layouts (dock nesting),
saved dock layouts with hotkeys, search bars inside the native Scenes and
Sources panels, and color coded scene names.

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

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		dockx::setObsReady(true); /* video docks may render now */
		dockx::panels::initAfterLoad();
		dockx::filters::rescanSoon();
		dockx::folders::rebuildSoon();
		dockx::folders::showFirstRun();
		dockx::hints::showFirstRun();
		dockx::sourcedocks::refreshAll();
		dockx::editpreview::refreshAll();
		dockx::missing::autoPopIfNeeded();
		dockx::monitors::installWatch();
		dockx::container::restoreFromState();
		dockx::preview::apply();
		dockx::edges::start();
		dockx::blocker::start();
		break;
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
		dockx::panels::autoSceneLayout();
		dockx::panels::refreshSoon();
		dockx::sourcedocks::refreshAll();
		dockx::editpreview::refreshAll();
		break;
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
		dockx::panels::refreshSoon();
		dockx::sourcedocks::refreshAll();
		dockx::editpreview::refreshAll();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		/* same danger window as startup: sources torn down + recreated
		   while the graphics thread runs; stop rendering user scenes */
		dockx::setObsReady(false);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		dockx::setObsReady(true);
		dockx::thumbs::invalidateAll(); /* uuids belong to the old collection */
		dockx::panels::refreshSoon();
		dockx::filters::rescanSoon();
		dockx::sourcedocks::refreshAll();
		dockx::editpreview::refreshAll();
		dockx::tagdock::refresh(); /* tags belong to the new collection */
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		/* super-profiles: bring the paired scene collection along */
		dockx::panels::followProfileLink();
		break;
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
		dockx::preview::onStudioModeEnabled();
		[[fallthrough]];
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
		dockx::folders::rebuildSoon();
		dockx::sourcedocks::refreshAll();
		dockx::editpreview::refreshAll();
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		/* OBS just replaced the app stylesheet; re-add the accent block */
		dockx::panels::applyChromeSoon();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		dockx::setObsReady(false);   /* no draws while OBS tears down */
		dockx::container::shutdown(); /* hand panels home + capture layout, before stateSave */
		dockx::stateSave();
		dockx::locks::unregisterHotkeys();
		dockx::tags::shutdownHotkeys(); /* after stateSave, which persists the bindings */
		dockx::tagdock::shutdown();
		dockx::editpreview::shutdown(); /* displays first, while graphics lives */
		dockx::sourcedocks::shutdown(); /* displays first, while graphics lives */
		dockx::filters::shutdown();
		dockx::folders::shutdown();
		dockx::placeholders::shutdown(); /* drops always-on-top from pinned windows */
		dockx::stats::shutdown();
		dockx::divider::shutdown();
		dockx::thumbs::shutdown();
		dockx::blocker::shutdown();
		dockx::edges::shutdown();
		dockx::panels::shutdown();
		break;
	default:
		break;
	}
}

static void tools_menu_clicked(void *)
{
	dockx::showDialog();
}

bool obs_module_load(void)
{
	obs_log(LOG_INFO, "DockX loaded (version %s)", PLUGIN_VERSION);
	dockx::stateLoad();
	dockx::filters::init();
	dockx::folders::createDock();
	dockx::stats::createDock();
	dockx::sourcedocks::createFromState();
	dockx::editpreview::createFromState();
	dockx::placeholders::createFromState();
	dockx::tagdock::createDock();
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_frontend_add_tools_menu_item("DockX", tools_menu_clicked, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "DockX unloaded");
}
