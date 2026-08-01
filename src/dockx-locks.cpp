/*
DockX for OBS Studio (by StrmrX) -- lock tools.
GPL v2, see plugin-main.cpp for the full notice.

Two kinds of lock, kept independent on purpose:
  - Dock layout: a hard lock (docks can't be dragged/floated at all) and a soft
    lock (docks still move, but a saved revert point snaps them back on demand).
  - Scene sources: lock every source in a scene (or all scenes) so nothing on
    the canvas can be nudged.

Same house rule as the rest of DockX: if anything looks off, do nothing. Never
crash OBS.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QMetaObject>

#include <functional>

namespace dockx {
namespace locks {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* ================= dock layout ================= */

bool hardLock()
{
	return state().hardLock;
}

void applyHardLock()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	const bool lock = state().hardLock;
	const auto docks = m->findChildren<QDockWidget *>();
	for (QDockWidget *d : docks) {
		if (!d)
			continue;
		QDockWidget::DockWidgetFeatures f = d->features();
		if (lock)
			f &= ~(QDockWidget::DockWidgetMovable |
			       QDockWidget::DockWidgetFloatable);
		else
			f |= (QDockWidget::DockWidgetMovable |
			      QDockWidget::DockWidgetFloatable);
		d->setFeatures(f);
	}
	obs_log(LOG_INFO, "dock hard lock %s", lock ? "on" : "off");
}

void setHardLock(bool on)
{
	state().hardLock = on;
	applyHardLock();
	stateSave();
}

bool hasLockPoint()
{
	return !state().lockPoint.isEmpty();
}

void setLockPoint()
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	state().lockPoint = m->saveState();
	stateSave();
	obs_log(LOG_INFO, "dock revert point saved");
}

bool revertToLockPoint()
{
	QMainWindow *m = mainWindow();
	if (!m || state().lockPoint.isEmpty())
		return false;
	/* keep an undo so "Undo apply" on the Layouts tab also undoes a revert */
	state().undoState = m->saveState();
	bool ok = m->restoreState(state().lockPoint);
	applyHardLock(); /* restoreState may re-show docks; reassert the freeze */
	obs_log(LOG_INFO, "reverted docks to lock point (%s)",
		ok ? "ok" : "restore reported failure");
	return ok;
}

/* ================= scene source locks ================= */

void lockCurrentScene(bool locked)
{
	obs_source_t *cur = obs_frontend_get_current_scene();
	if (!cur)
		return;
	const QString uuid = QString::fromUtf8(obs_source_get_uuid(cur));
	obs_source_release(cur);
	loadouts::lockScene(uuid, locked);
}

void lockAllScenes(bool locked)
{
	loadouts::lockAll(locked);
}

void lockScenes(const QStringList &uuids, bool locked)
{
	for (const QString &u : uuids)
		loadouts::lockScene(u, locked);
}

/* ================= hotkeys ================= */

/* every callback hops onto the UI thread: dock restore and scene item edits
   must not run on the hotkey thread */
static void queueOnMain(std::function<void()> fn)
{
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	QMetaObject::invokeMethod(m, [fn]() { fn(); }, Qt::QueuedConnection);
}

static void revertCb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueOnMain([]() { revertToLockPoint(); });
}

static void lockSceneCb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueOnMain([]() { lockCurrentScene(true); });
}

static void unlockSceneCb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueOnMain([]() { lockCurrentScene(false); });
}

static void hardLockCb(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		queueOnMain([]() { setHardLock(!state().hardLock); });
}

void registerHotkeys()
{
	State &s = state();
	if (s.hkRevert == OBS_INVALID_HOTKEY_ID)
		s.hkRevert = obs_hotkey_register_frontend(
			"dockx_revert_layout", "DockX: Revert docks to lock point",
			revertCb, nullptr);
	if (s.hkLockScene == OBS_INVALID_HOTKEY_ID)
		s.hkLockScene = obs_hotkey_register_frontend(
			"dockx_lock_scene", "DockX: Lock all sources in current scene",
			lockSceneCb, nullptr);
	if (s.hkUnlockScene == OBS_INVALID_HOTKEY_ID)
		s.hkUnlockScene = obs_hotkey_register_frontend(
			"dockx_unlock_scene",
			"DockX: Unlock all sources in current scene", unlockSceneCb,
			nullptr);
	if (s.hkHardLock == OBS_INVALID_HOTKEY_ID)
		s.hkHardLock = obs_hotkey_register_frontend(
			"dockx_hard_lock", "DockX: Lock docks in place (toggle)",
			hardLockCb, nullptr);
}

void unregisterHotkeys()
{
	State &s = state();
	obs_hotkey_id ids[] = {s.hkRevert, s.hkLockScene, s.hkUnlockScene, s.hkHardLock};
	for (obs_hotkey_id id : ids)
		if (id != OBS_INVALID_HOTKEY_ID)
			obs_hotkey_unregister(id);
	s.hkRevert = s.hkLockScene = s.hkUnlockScene = s.hkHardLock =
		OBS_INVALID_HOTKEY_ID;
}

static void loadOne(obs_data_t *d, const char *key, obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return;
	obs_data_array_t *hk = obs_data_get_array(d, key);
	if (hk) {
		obs_hotkey_load(id, hk);
		obs_data_array_release(hk);
	}
}

void loadHotkeys(obs_data_t *d)
{
	const State &s = state();
	loadOne(d, "hk_revert", s.hkRevert);
	loadOne(d, "hk_lock_scene", s.hkLockScene);
	loadOne(d, "hk_unlock_scene", s.hkUnlockScene);
	loadOne(d, "hk_hard_lock", s.hkHardLock);
}

static void saveOne(obs_data_t *d, const char *key, obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return;
	obs_data_array_t *hk = obs_hotkey_save(id);
	if (hk) {
		obs_data_set_array(d, key, hk);
		obs_data_array_release(hk);
	}
}

void saveHotkeys(obs_data_t *d)
{
	const State &s = state();
	saveOne(d, "hk_revert", s.hkRevert);
	saveOne(d, "hk_lock_scene", s.hkLockScene);
	saveOne(d, "hk_unlock_scene", s.hkUnlockScene);
	saveOne(d, "hk_hard_lock", s.hkHardLock);
}

} // namespace locks
} // namespace dockx
