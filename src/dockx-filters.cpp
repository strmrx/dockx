/*
DockX for OBS Studio (by StrmrX) -- filter toggle hotkeys.
GPL v2, see plugin-main.cpp for the full notice.

Every filter on every source gets a toggle hotkey, registered ON the parent
source so OBS saves the bindings inside the scene collection. We reconcile
registrations whenever sources or filters change. Same prime directive as the
rest of DockX: if anything looks off, do nothing. Never crash OBS.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

#include <algorithm>

namespace dockx {
namespace filters {

struct HotkeyCtx {
	obs_weak_source_t *weak;
	QByteArray filter;
};

struct Reg {
	QString sourceName;
	QString filterName;
	obs_hotkey_id id = OBS_INVALID_HOTKEY_ID;
	HotkeyCtx *ctx = nullptr;
};

static QHash<QString, Reg> g_regs;     /* "<source uuid>\n<filter name>" -> reg */
static QSet<QString> g_watchedSources; /* uuids we connected filter signals on */
static QPointer<QTimer> g_timer;
static bool g_shutdown = false;

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

static void toggleCb(void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	HotkeyCtx *ctx = static_cast<HotkeyCtx *>(data);
	obs_source_t *src = obs_weak_source_get_source(ctx->weak);
	if (!src)
		return;
	obs_source_t *f = obs_source_get_filter_by_name(src, ctx->filter.constData());
	if (f) {
		obs_source_set_enabled(f, !obs_source_enabled(f));
		obs_source_release(f);
	}
	obs_source_release(src);
}

static void freeReg(Reg &r)
{
	if (r.ctx) {
		/* only unregister while the source is alive; a dead source
		   already took its hotkeys down with it */
		obs_source_t *src = obs_weak_source_get_source(r.ctx->weak);
		if (src) {
			if (r.id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(r.id);
			obs_source_release(src);
		}
		obs_weak_source_release(r.ctx->weak);
		delete r.ctx;
		r.ctx = nullptr;
	}
}

static void filterSignal(void *, calldata_t *)
{
	rescanSoon();
}

static void enumFilter(obs_source_t *parent, obs_source_t *filter, void *param)
{
	QSet<QString> *found = static_cast<QSet<QString> *>(param);
	const char *uuid = obs_source_get_uuid(parent);
	const char *fname = obs_source_get_name(filter);
	const char *sname = obs_source_get_name(parent);
	if (!uuid || !fname || !*fname || !sname)
		return;
	const QString key = QString::fromUtf8(uuid) + QChar('\n') + QString::fromUtf8(fname);
	found->insert(key);
	auto it = g_regs.find(key);
	if (it != g_regs.end()) {
		it->sourceName = QString::fromUtf8(sname); /* follow source renames */
		return;
	}
	HotkeyCtx *ctx = new HotkeyCtx{obs_source_get_weak_source(parent), QByteArray(fname)};
	const QByteArray hname = QString("dockx_filter.%1").arg(fname).toUtf8();
	const QByteArray hdesc = QString("DockX: toggle filter \"%1\"").arg(fname).toUtf8();
	Reg r;
	r.sourceName = QString::fromUtf8(sname);
	r.filterName = QString::fromUtf8(fname);
	r.ctx = ctx;
	r.id = obs_hotkey_register_source(parent, hname.constData(), hdesc.constData(), toggleCb,
					  ctx);
	g_regs.insert(key, r);
}

static bool enumSource(void *param, obs_source_t *src)
{
	const char *uuid = obs_source_get_uuid(src);
	if (!uuid)
		return true;
	const QString uid = QString::fromUtf8(uuid);
	if (!g_watchedSources.contains(uid)) {
		signal_handler_t *sh = obs_source_get_signal_handler(src);
		if (sh) {
			signal_handler_connect(sh, "filter_add", filterSignal, nullptr);
			signal_handler_connect(sh, "filter_remove", filterSignal, nullptr);
		}
		g_watchedSources.insert(uid);
	}
	obs_source_enum_filters(src, enumFilter, param);
	return true;
}

static void clearAll()
{
	for (Reg &r : g_regs)
		freeReg(r);
	g_regs.clear();
}

static void rescanNow()
{
	if (g_shutdown)
		return;
	if (!state().filterHotkeys) {
		clearAll();
		return;
	}
	QSet<QString> found;
	obs_enum_sources(enumSource, &found);
	obs_enum_scenes(enumSource, &found);
	for (auto it = g_regs.begin(); it != g_regs.end();) {
		if (!found.contains(it.key())) {
			freeReg(it.value());
			it = g_regs.erase(it);
		} else {
			++it;
		}
	}
}

void rescanSoon()
{
	if (g_shutdown)
		return;
	QMainWindow *m = mainWindow();
	if (!m)
		return;
	/* source signals can fire off the UI thread; the timer must not */
	QMetaObject::invokeMethod(
		m,
		[]() {
			if (g_shutdown)
				return;
			if (!g_timer) {
				QMainWindow *mw = mainWindow();
				if (!mw)
					return;
				g_timer = new QTimer(mw);
				g_timer->setSingleShot(true);
				g_timer->setInterval(250);
				QObject::connect(g_timer, &QTimer::timeout, []() { rescanNow(); });
			}
			g_timer->start();
		},
		Qt::QueuedConnection);
}

void applyEnabled()
{
	rescanSoon();
}

QList<Entry> entries()
{
	QList<Entry> out;
	for (const Reg &r : g_regs)
		out.append({r.sourceName, r.filterName, r.id});
	std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
		const int c = a.sourceName.compare(b.sourceName, Qt::CaseInsensitive);
		if (c != 0)
			return c < 0;
		return a.filterName.compare(b.filterName, Qt::CaseInsensitive) < 0;
	});
	return out;
}

static void globalSignal(void *, calldata_t *)
{
	rescanSoon();
}

void init()
{
	signal_handler_t *sh = obs_get_signal_handler();
	if (!sh)
		return;
	signal_handler_connect(sh, "source_create", globalSignal, nullptr);
	signal_handler_connect(sh, "source_destroy", globalSignal, nullptr);
	signal_handler_connect(sh, "source_rename", globalSignal, nullptr);
}

void shutdown()
{
	g_shutdown = true;
	if (g_timer)
		g_timer->stop();
	signal_handler_t *sh = obs_get_signal_handler();
	if (sh) {
		signal_handler_disconnect(sh, "source_create", globalSignal, nullptr);
		signal_handler_disconnect(sh, "source_destroy", globalSignal, nullptr);
		signal_handler_disconnect(sh, "source_rename", globalSignal, nullptr);
	}
	clearAll();
}

} // namespace filters
} // namespace dockx
