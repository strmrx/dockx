/*
DockX for OBS Studio (by StrmrX) -- live source docks.
GPL v2, see plugin-main.cpp for the full notice.

Renders any source, scene, the Preview (studio mode) or the Program inside a
regular OBS dock via an obs_display bound to a native Qt widget. Sources are
held as weak references and resolved by name on the UI thread; the draw
callback (graphics thread) only touches libobs.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDockWidget>
#include <QMainWindow>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>

#include <mutex>
#include <vector>

namespace dockx {
namespace sourcedocks {

static bool g_shutdown = false;

static QString dockIdFor(int id)
{
	return QString("dockx_source_%1").arg(id);
}

class SourceDockView : public QWidget {
public:
	int id;
	int kind;
	QString sourceName;

	SourceDockView(int id_, int kind_, const QString &name)
		: QWidget(nullptr),
		  id(id_),
		  kind(kind_),
		  sourceName(name)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(80, 45);
	}

	~SourceDockView() override
	{
		destroyDisplay();
		setWeak(nullptr);
	}

	QPaintEngine *paintEngine() const override { return nullptr; }

	void setWeak(obs_weak_source_t *w)
	{
		std::lock_guard<std::mutex> lock(weakMutex);
		if (weak)
			obs_weak_source_release(weak);
		weak = w;
	}

	/* caller releases; safe from any thread */
	obs_source_t *lockSource()
	{
		std::lock_guard<std::mutex> lock(weakMutex);
		return weak ? obs_weak_source_get_source(weak) : nullptr;
	}

	/* UI thread: point the weak ref at the right source. A live weak ref
	   survives renames, so if the name lookup fails but the old source is
	   still alive we keep it and adopt its new name instead of going blank. */
	void resolve()
	{
		if (kind == KIND_PROGRAM)
			return; /* rendered straight off the main texture */
		obs_source_t *src = nullptr;
		if (kind == KIND_PREVIEW) {
			if (obs_frontend_preview_program_mode_active())
				src = obs_frontend_get_current_preview_scene();
			setWeak(src ? obs_source_get_weak_source(src) : nullptr);
			if (src)
				obs_source_release(src);
			return;
		}
		src = obs_get_source_by_name(sourceName.toUtf8().constData());
		if (src) {
			setWeak(obs_source_get_weak_source(src));
			obs_source_release(src);
			return;
		}
		obs_source_t *alive = lockSource();
		if (alive) {
			const char *n = obs_source_get_name(alive);
			if (n && *n)
				adoptName(QString::fromUtf8(n));
			obs_source_release(alive);
			return; /* renamed, not gone; keep rendering it */
		}
		setWeak(nullptr);
	}

	void destroyDisplay()
	{
		if (!display)
			return;
		obs_display_remove_draw_callback(display, &SourceDockView::drawCb, this);
		obs_display_destroy(display);
		display = nullptr;
	}

	void clearWeak() { setWeak(nullptr); }

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		ensureDisplay();
		if (display)
			obs_display_set_enabled(display, true);
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		if (display)
			obs_display_set_enabled(display, false); /* no GPU work while hidden */
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (display) {
			const qreal dpr = devicePixelRatioF();
			obs_display_resize(display, (uint32_t)(width() * dpr),
					   (uint32_t)(height() * dpr));
		}
	}

private:
	obs_display_t *display = nullptr;
	obs_weak_source_t *weak = nullptr;
	std::mutex weakMutex;

	void ensureDisplay()
	{
		if (display || g_shutdown)
			return;
		const qreal dpr = devicePixelRatioF();
		gs_init_data info = {};
		info.cx = (uint32_t)(width() > 0 ? width() * dpr : 8);
		info.cy = (uint32_t)(height() > 0 ? height() * dpr : 8);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#ifdef _WIN32
		info.window.hwnd = reinterpret_cast<void *>(winId());
#else
#error "source docks: window handle wiring needed for this platform"
#endif
		display = obs_display_create(&info, 0x151515);
		if (display)
			obs_display_add_draw_callback(display, &SourceDockView::drawCb,
						      this);
		else
			obs_log(LOG_WARNING, "source dock %d: display create failed", id);
	}

	void adoptName(const QString &n)
	{
		if (n == sourceName)
			return;
		sourceName = n;
		for (SourceDockEntry &e : state().sourceDocks)
			if (e.id == id)
				e.sourceName = n;
		stateSave();
		if (QWidget *p = parentWidget())
			p->setWindowTitle(n);
	}

	/* graphics thread: libobs calls only */
	static void drawCb(void *param, uint32_t cx, uint32_t cy)
	{
		SourceDockView *v = static_cast<SourceDockView *>(param);
		obs_source_t *src = nullptr;
		uint32_t w, h;
		if (v->kind == KIND_PROGRAM) {
			obs_video_info ovi;
			if (!obs_get_video_info(&ovi))
				return;
			w = ovi.base_width;
			h = ovi.base_height;
		} else {
			src = v->lockSource();
			if (!src)
				return;
			w = obs_source_get_width(src);
			h = obs_source_get_height(src);
			if (!w || !h) {
				obs_source_release(src);
				return;
			}
		}
		const float scale =
			qMin((float)cx / (float)w, (float)cy / (float)h);
		const int vw = (int)(scale * (float)w);
		const int vh = (int)(scale * (float)h);
		const int vx = ((int)cx - vw) / 2;
		const int vy = ((int)cy - vh) / 2;
		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		if (src) {
			obs_source_video_render(src);
			obs_source_release(src);
		} else {
			obs_render_main_texture();
		}
		gs_projection_pop();
		gs_viewport_pop();
	}
};

static std::vector<SourceDockView *> g_views;

static QString titleFor(const SourceDockEntry &e)
{
	if (e.kind == KIND_PROGRAM)
		return QStringLiteral("Program");
	if (e.kind == KIND_PREVIEW)
		return QStringLiteral("Preview");
	return e.sourceName;
}

static SourceDockView *registerView(const SourceDockEntry &e)
{
	SourceDockView *v = new SourceDockView(e.id, e.kind, e.sourceName);
	if (!obs_frontend_add_dock_by_id(dockIdFor(e.id).toUtf8().constData(),
					 titleFor(e).toUtf8().constData(), v)) {
		obs_log(LOG_WARNING, "could not register source dock %d", e.id);
		delete v;
		return nullptr;
	}
	g_views.push_back(v);
	return v;
}

void createFromState()
{
	for (const SourceDockEntry &e : state().sourceDocks)
		registerView(e);
}

void refreshAll()
{
	if (g_shutdown)
		return;
	for (SourceDockView *v : g_views)
		v->resolve();
}

void addDock(int kind, const QString &sourceName)
{
	SourceDockEntry e;
	e.id = state().nextSourceDockId++;
	e.kind = kind;
	e.sourceName = sourceName;
	state().sourceDocks.push_back(e);
	stateSave();
	SourceDockView *v = registerView(e);
	if (!v)
		return;
	v->resolve();
	/* pop the new dock open so it visibly appears */
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(dockIdFor(e.id)) : nullptr;
	if (dock) {
		dock->setVisible(true);
		dock->raise();
	}
}

void removeDock(int id)
{
	for (auto it = g_views.begin(); it != g_views.end(); ++it) {
		if ((*it)->id == id) {
			g_views.erase(it);
			break;
		}
	}
	/* removing the dock destroys the wrapper AND our view widget */
	obs_frontend_remove_dock(dockIdFor(id).toUtf8().constData());
	auto &v = state().sourceDocks;
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (it->id == id) {
			v.erase(it);
			break;
		}
	}
	stateSave();
}

void shutdown()
{
	g_shutdown = true;
	/* displays and refs must die while graphics is still alive; the widgets
	   themselves are torn down later with their QDockWidget parents */
	for (SourceDockView *v : g_views) {
		v->destroyDisplay();
		v->clearWeak();
	}
	g_views.clear();
}

} // namespace sourcedocks
} // namespace dockx
