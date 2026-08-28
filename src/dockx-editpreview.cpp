/*
DockX for OBS Studio (by StrmrX) -- the editable Preview dock.
GPL v2, see plugin-main.cpp for the full notice.

OBS gives you canvas editing (click a source, drag it, snap it) in exactly ONE
place: its single fixed main preview. This renders the current scene inside a
normal DockX dock and rebuilds that editing on top -- hit-test a click to the
topmost source under it, drag to move, snap to the canvas edges and center --
so you can edit your scene from a movable, resizable dock even while OBS's main
preview is collapsed. The scene is rendered straight through its own obs_display
(not the shared main texture), so it shows video whether or not OBS's own main
preview is enabled -- unlike a passive Program dock, which goes black off-stream.

First slice: select + move + snap, current scene. Transform handles, resize,
rotate, and multi-select build on this same rendering + hit-test base.

All scene mutation happens on the UI thread (the mouse handlers); the draw
callback (graphics thread) only reads, to render the scene + the selection
overlay. Sources are held as weak references and resolved on the UI thread.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <QDockWidget>
#include <QHideEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QWidget>

#include <cmath>
#include <mutex>
#include <vector>

namespace dockx {
namespace editpreview {

static bool g_shutdown = false;

static QString dockIdFor(int id)
{
	return QString("dockx_edit_%1").arg(id);
}

/* vec4_from_rgba reads the low byte as red: 0xAABBGGRR. A bright teal selection
   outline + white snap guides both read against most scene content. */
static const uint32_t COLOR_SELECT = 0xFFA6E22D; /* R2D G E2 B A6 A FF */
static const uint32_t COLOR_SNAP = 0xFFFFFFFF;

static vec2 mk(float x, float y)
{
	vec2 v;
	v.x = x;
	v.y = y;
	return v;
}

/* the native paint surface: renders the current scene + the editing overlay */
class EditWidget : public QWidget {
public:
	int id;

	EditWidget(int id_, QWidget *parent) : QWidget(parent), id(id_)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		setMinimumSize(160, 90);
		setMouseTracking(true);
		setFocusPolicy(Qt::ClickFocus);
	}

	~EditWidget() override { teardown(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	/* release GPU + libobs handles while graphics is still alive; idempotent */
	void teardown()
	{
		endDrag();
		destroyDisplay();
		setScene(nullptr);
	}

	/* UI thread: point the dock at the current program scene */
	void refresh()
	{
		obs_source_t *scene = obs_frontend_get_current_scene();
		setScene(scene ? obs_source_get_weak_source(scene) : nullptr);
		if (scene)
			obs_source_release(scene);
	}

	void setScene(obs_weak_source_t *w)
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (sceneWeak)
			obs_weak_source_release(sceneWeak);
		sceneWeak = w;
	}

	/* caller releases; safe from any thread */
	obs_source_t *lockScene()
	{
		std::lock_guard<std::mutex> lock(mtx);
		return sceneWeak ? obs_weak_source_get_source(sceneWeak) : nullptr;
	}

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
			obs_display_set_enabled(display, false); /* no GPU work hidden */
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

	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		float cx, cy;
		if (!mapToCanvas(e->position(), cx, cy))
			return;
		const bool add = (e->modifiers() & Qt::ControlModifier) != 0;
		/* if the click lands inside something already selected, drag THAT
		   selection as-is -- so a source picked in the Sources list (or a
		   lower layer) moves even when another source overlaps the click.
		   Only a plain click on unselected space re-hit-tests to the top item. */
		if (!add && pointInSelection(cx, cy)) {
			beginDrag(cx, cy);
			setFocus();
			return;
		}
		obs_sceneitem_t *hit = hitTest(cx, cy); /* owns a ref, or null */
		applySelection(hit, add);
		if (hit)
			obs_sceneitem_release(hit);
		beginDrag(cx, cy);
		setFocus();
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!dragging || !(e->buttons() & Qt::LeftButton))
			return;
		float cx, cy;
		if (!mapToCanvas(e->position(), cx, cy))
			return;
		dragTo(cx, cy);
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			endDrag();
	}

private:
	obs_display_t *display = nullptr;
	obs_weak_source_t *sceneWeak = nullptr;
	std::mutex mtx;

	/* drag state (UI thread only) */
	bool dragging = false;
	float grabX = 0, grabY = 0;
	float selL = 0, selT = 0, selR = 0, selB = 0; /* selection bbox at drag start */
	struct Held {
		obs_sceneitem_t *item;
		vec2 start;
	};
	std::vector<Held> held;

	/* snap guides for the overlay (written on the UI thread, read on the
	   graphics thread; a benign one-frame race at worst, no crash) */
	bool snapX = false, snapY = false;
	float snapXpos = 0, snapYpos = 0;

	/* ---- display lifecycle ---- */

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
#error "editable preview: window handle wiring needed for this platform"
#endif
		display = obs_display_create(&info, 0x151515);
		if (display)
			obs_display_add_draw_callback(display, &EditWidget::drawCb, this);
		else
			obs_log(LOG_WARNING, "edit dock %d: display create failed", id);
	}

	void destroyDisplay()
	{
		if (!display)
			return;
		obs_display_remove_draw_callback(display, &EditWidget::drawCb, this);
		obs_display_destroy(display);
		display = nullptr;
	}

	/* ---- coordinate mapping ---- */

	/* letterbox metrics: device px per canvas px + the black-bar offsets */
	bool metrics(float &scale, float &offX, float &offY, uint32_t &cw, uint32_t &ch)
	{
		obs_source_t *scene = lockScene();
		if (!scene)
			return false;
		cw = obs_source_get_width(scene);
		ch = obs_source_get_height(scene);
		obs_source_release(scene);
		if (!cw || !ch)
			return false;
		const qreal dpr = devicePixelRatioF();
		const float vw = (float)(width() * dpr);
		const float vh = (float)(height() * dpr);
		scale = qMin(vw / (float)cw, vh / (float)ch);
		if (scale <= 0.0f)
			return false;
		offX = (vw - scale * (float)cw) / 2.0f;
		offY = (vh - scale * (float)ch) / 2.0f;
		return true;
	}

	bool mapToCanvas(const QPointF &pos, float &cx, float &cy)
	{
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return false;
		const qreal dpr = devicePixelRatioF();
		cx = ((float)(pos.x() * dpr) - offX) / scale;
		cy = ((float)(pos.y() * dpr) - offY) / scale;
		return true;
	}

	/* ~10 device px snap radius (OBS's default), expressed in canvas px */
	float snapRadius()
	{
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return 0.0f;
		return 10.0f / scale;
	}

	/* ---- hit testing ---- */

	static bool collectAll(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		auto *v = static_cast<std::vector<obs_sceneitem_t *> *>(param);
		obs_sceneitem_addref(item);
		v->push_back(item);
		return true;
	}

	static bool pointInItem(obs_sceneitem_t *item, float cx, float cy)
	{
		matrix4 m, inv;
		obs_sceneitem_get_box_transform(item, &m);
		if (!matrix4_inv(&inv, &m))
			return false;
		vec3 p, r;
		vec3_set(&p, cx, cy, 0.0f);
		vec3_transform(&r, &p, &inv);
		return r.x >= 0.0f && r.x <= 1.0f && r.y >= 0.0f && r.y <= 1.0f;
	}

	/* is the canvas point inside any currently-selected (draggable) item? */
	struct SelHit {
		float cx, cy;
		bool hit;
	};
	static bool selHitCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		auto *s = static_cast<SelHit *>(param);
		if (!s->hit && obs_sceneitem_selected(item) &&
		    obs_sceneitem_visible(item) && !obs_sceneitem_locked(item) &&
		    pointInItem(item, s->cx, s->cy))
			s->hit = true;
		return true;
	}
	bool pointInSelection(float cx, float cy)
	{
		obs_source_t *sceneSrc = lockScene();
		if (!sceneSrc)
			return false;
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		SelHit s{cx, cy, false};
		if (scene)
			obs_scene_enum_items(scene, selHitCb, &s);
		obs_source_release(sceneSrc);
		return s.hit;
	}

	/* topmost unlocked, visible item under the canvas point; returns a ref */
	obs_sceneitem_t *hitTest(float cx, float cy)
	{
		obs_source_t *sceneSrc = lockScene();
		if (!sceneSrc)
			return nullptr;
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		std::vector<obs_sceneitem_t *> items;
		if (scene)
			obs_scene_enum_items(scene, collectAll, &items);
		obs_sceneitem_t *found = nullptr;
		/* enum walks bottom to top; reverse so the topmost wins */
		for (auto it = items.rbegin(); it != items.rend(); ++it) {
			obs_sceneitem_t *item = *it;
			if (!found && !obs_sceneitem_locked(item) &&
			    obs_sceneitem_visible(item) && pointInItem(item, cx, cy))
				found = item; /* keep this ref for the caller */
			else
				obs_sceneitem_release(item);
		}
		obs_source_release(sceneSrc);
		return found;
	}

	/* ---- selection ---- */

	static bool deselectCb(obs_scene_t *, obs_sceneitem_t *item, void *)
	{
		obs_sceneitem_select(item, false);
		return true;
	}

	void applySelection(obs_sceneitem_t *hit, bool add)
	{
		if (!add) {
			obs_source_t *sceneSrc = lockScene();
			if (sceneSrc) {
				obs_scene_t *scene = obs_scene_from_source(sceneSrc);
				if (scene)
					obs_scene_enum_items(scene, deselectCb, nullptr);
				obs_source_release(sceneSrc);
			}
		}
		if (hit)
			obs_sceneitem_select(hit, true);
	}

	/* ---- drag + snap ---- */

	static bool collectDrag(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		auto *held = static_cast<std::vector<Held> *>(param);
		if (obs_sceneitem_selected(item) && !obs_sceneitem_locked(item)) {
			obs_sceneitem_addref(item);
			Held h;
			h.item = item;
			obs_sceneitem_get_pos(item, &h.start);
			held->push_back(h);
		}
		return true;
	}

	void beginDrag(float cx, float cy)
	{
		endDrag();
		grabX = cx;
		grabY = cy;
		obs_source_t *sceneSrc = lockScene();
		if (sceneSrc) {
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene)
				obs_scene_enum_items(scene, collectDrag, &held);
			obs_source_release(sceneSrc);
		}
		dragging = !held.empty();
		selL = selT = 1e30f;
		selR = selB = -1e30f;
		const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		for (Held &h : held) {
			matrix4 m;
			obs_sceneitem_get_box_transform(h.item, &m);
			for (const auto &u : uv) {
				vec3 v, r;
				vec3_set(&v, u[0], u[1], 0.0f);
				vec3_transform(&r, &v, &m);
				selL = qMin(selL, r.x);
				selT = qMin(selT, r.y);
				selR = qMax(selR, r.x);
				selB = qMax(selB, r.y);
			}
		}
	}

	/* nudge dx/dy so the selection bbox latches onto a canvas edge/center */
	void computeSnap(float &dx, float &dy)
	{
		snapX = snapY = false;
		if (held.empty())
			return;
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return;
		const float snap = 10.0f / scale;
		const float W = (float)cw, H = (float)ch;
		{
			const float ex[3] = {selL + dx, selR + dx, (selL + selR) * 0.5f + dx};
			const float tx[3] = {0.0f, W, W * 0.5f};
			float best = snap, adj = 0, gpos = 0;
			bool got = false;
			for (int i = 0; i < 3; i++) {
				const float nd = tx[i] - ex[i];
				if (fabsf(nd) < best) {
					best = fabsf(nd);
					adj = nd;
					gpos = tx[i];
					got = true;
				}
			}
			if (got) {
				dx += adj;
				snapX = true;
				snapXpos = gpos;
			}
		}
		{
			const float ey[3] = {selT + dy, selB + dy, (selT + selB) * 0.5f + dy};
			const float ty[3] = {0.0f, H, H * 0.5f};
			float best = snap, adj = 0, gpos = 0;
			bool got = false;
			for (int i = 0; i < 3; i++) {
				const float nd = ty[i] - ey[i];
				if (fabsf(nd) < best) {
					best = fabsf(nd);
					adj = nd;
					gpos = ty[i];
					got = true;
				}
			}
			if (got) {
				dy += adj;
				snapY = true;
				snapYpos = gpos;
			}
		}
	}

	void dragTo(float cx, float cy)
	{
		float dx = cx - grabX;
		float dy = cy - grabY;
		computeSnap(dx, dy);
		for (Held &h : held) {
			vec2 p;
			p.x = h.start.x + dx;
			p.y = h.start.y + dy;
			obs_sceneitem_set_pos(h.item, &p);
		}
	}

	void endDrag()
	{
		for (Held &h : held)
			obs_sceneitem_release(h.item);
		held.clear();
		dragging = false;
		snapX = snapY = false;
	}

	/* ---- rendering (graphics thread) ---- */

	static void drawLineStrip(const std::vector<vec2> &pts, uint32_t rgba)
	{
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		if (!solid)
			return;
		gs_eparam_t *colorParam = gs_effect_get_param_by_name(solid, "color");
		vec4 col;
		vec4_from_rgba(&col, rgba);
		gs_effect_set_vec4(colorParam, &col);
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		const size_t passes = gs_technique_begin(tech);
		for (size_t p = 0; p < passes; p++) {
			if (!gs_technique_begin_pass(tech, p))
				continue;
			gs_render_start(true);
			for (const vec2 &v : pts)
				gs_vertex2f(v.x, v.y);
			gs_render_stop(GS_LINESTRIP);
			gs_technique_end_pass(tech);
		}
		gs_technique_end(tech);
	}

	static bool collectSelectedBoxes(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item)) {
			auto *v = static_cast<std::vector<matrix4> *>(param);
			matrix4 m;
			obs_sceneitem_get_box_transform(item, &m);
			v->push_back(m);
		}
		return true;
	}

	void drawOverlay(obs_source_t *sceneSrc, uint32_t w, uint32_t h)
	{
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		if (scene) {
			std::vector<matrix4> boxes;
			obs_scene_enum_items(scene, collectSelectedBoxes, &boxes);
			const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (const matrix4 &m : boxes) {
				std::vector<vec2> pts(5);
				for (int i = 0; i < 4; i++) {
					vec3 v, r;
					vec3_set(&v, uv[i][0], uv[i][1], 0.0f);
					vec3_transform(&r, &v, &m);
					pts[i] = mk(r.x, r.y);
				}
				pts[4] = pts[0];
				drawLineStrip(pts, COLOR_SELECT);
			}
		}
		if (snapX) {
			std::vector<vec2> p{mk(snapXpos, 0.0f), mk(snapXpos, (float)h)};
			drawLineStrip(p, COLOR_SNAP);
		}
		if (snapY) {
			std::vector<vec2> p{mk(0.0f, snapYpos), mk((float)w, snapYpos)};
			drawLineStrip(p, COLOR_SNAP);
		}
	}

	static void drawCb(void *param, uint32_t cx, uint32_t cy)
	{
		EditWidget *self = static_cast<EditWidget *>(param);
		obs_source_t *scene = self->lockScene();
		if (!scene)
			return;
		const uint32_t w = obs_source_get_width(scene);
		const uint32_t h = obs_source_get_height(scene);
		if (!w || !h) {
			obs_source_release(scene);
			return;
		}
		const float scale = qMin((float)cx / (float)w, (float)cy / (float)h);
		const int vw = (int)(scale * (float)w);
		const int vh = (int)(scale * (float)h);
		const int vx = ((int)cx - vw) / 2;
		const int vy = ((int)cy - vh) / 2;
		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		obs_source_video_render(scene);
		self->drawOverlay(scene, w, h);
		gs_projection_pop();
		gs_viewport_pop();
		obs_source_release(scene);
	}
};

static std::vector<EditWidget *> g_widgets;

static EditWidget *registerDock(int id)
{
	EditWidget *w = new EditWidget(id, nullptr);
	if (!obs_frontend_add_dock_by_id(dockIdFor(id).toUtf8().constData(),
					 "Editable preview", w)) {
		obs_log(LOG_WARNING, "could not register edit dock %d", id);
		delete w;
		return nullptr;
	}
	g_widgets.push_back(w);
	return w;
}

void createFromState()
{
	for (int id : state().editDocks)
		registerDock(id);
}

void refreshAll()
{
	if (g_shutdown)
		return;
	for (EditWidget *w : g_widgets)
		w->refresh();
}

void addDock()
{
	const int id = state().nextEditDockId++;
	state().editDocks.push_back(id);
	stateSave();
	EditWidget *w = registerDock(id);
	if (!w)
		return;
	w->refresh();
	/* pop it open so the new dock visibly appears */
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(dockIdFor(id)) : nullptr;
	if (dock) {
		dock->setVisible(true);
		dock->raise();
	}
}

void removeDock(int id)
{
	for (auto it = g_widgets.begin(); it != g_widgets.end(); ++it) {
		if ((*it)->id == id) {
			(*it)->teardown();
			g_widgets.erase(it);
			break;
		}
	}
	/* removing the dock destroys the wrapper AND our widget */
	obs_frontend_remove_dock(dockIdFor(id).toUtf8().constData());
	auto &v = state().editDocks;
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (*it == id) {
			v.erase(it);
			break;
		}
	}
	stateSave();
}

/* raise every editable dock (a closed dock stays closed forever in OBS); false
   when none exist so the caller can offer to add one */
bool showDocks()
{
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!m)
		return false;
	bool any = false;
	for (int id : state().editDocks) {
		QDockWidget *dock = m->findChild<QDockWidget *>(dockIdFor(id));
		if (dock) {
			dock->setVisible(true);
			dock->raise();
			any = true;
		}
	}
	return any;
}

QList<int> dockIds()
{
	QList<int> ids;
	for (int id : state().editDocks)
		ids.push_back(id);
	return ids;
}

void shutdown()
{
	g_shutdown = true;
	/* displays + refs must die while graphics is still alive; the widgets
	   themselves are torn down later with their QDockWidget parents */
	for (EditWidget *w : g_widgets)
		w->teardown();
	g_widgets.clear();
}

} // namespace editpreview
} // namespace dockx
