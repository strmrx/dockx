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
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QVBoxLayout>
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

static const float PI_F = 3.14159265358979323846f;

/* resize handles, in unit-square (UV) coords: gx/gy = the grabbed point, ax/ay =
   the anchor (opposite point, kept fixed while dragging), sx/sy = which axes this
   handle scales. Index order (used by the hover-cursor map):
   0 TL  1 TC  2 TR  3 MR  4 BR  5 BC  6 BL  7 ML */
struct HandleDef {
	float gx, gy, ax, ay;
	bool sx, sy;
};
static const HandleDef kHandles[8] = {
	{0.0f, 0.0f, 1.0f, 1.0f, true, true},   /* TL */
	{0.5f, 0.0f, 0.5f, 1.0f, false, true},  /* TC */
	{1.0f, 0.0f, 0.0f, 1.0f, true, true},   /* TR */
	{1.0f, 0.5f, 0.0f, 0.5f, true, false},  /* MR */
	{1.0f, 1.0f, 0.0f, 0.0f, true, true},   /* BR */
	{0.5f, 1.0f, 0.5f, 0.0f, false, true},  /* BC */
	{0.0f, 1.0f, 1.0f, 0.0f, true, true},   /* BL */
	{0.0f, 0.5f, 1.0f, 0.5f, true, false},  /* ML */
};
/* half-size of a handle square + how far the rotate handle stands off the top
   edge + the grab radius, all in device px (constant on screen at any zoom) */
static const float HANDLE_HALF_PX = 5.0f;
static const float ROT_OFFSET_PX = 24.0f;
static const float GRAB_PX = 11.0f;

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
		endInteraction();
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
		const qreal dpr = devicePixelRatioF();
		const float mdx = (float)(e->position().x() * dpr);
		const float mdy = (float)(e->position().y() * dpr);
		/* a transform handle on a single selected, unlocked item wins first:
		   the corner/edge squares resize it, the stalk above it rotates it. */
		if (!add) {
			obs_sceneitem_t *one = singleSelected(); /* owns a ref, or null */
			if (one) {
				const int h = hitHandle(one, mdx, mdy);
				if (h == -2) {
					beginRotate(one, cx, cy);
					obs_sceneitem_release(one);
					setFocus();
					return;
				}
				if (h >= 0) {
					beginResize(one, h);
					obs_sceneitem_release(one);
					setFocus();
					return;
				}
				obs_sceneitem_release(one);
			}
		}
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
		float cx, cy;
		const bool ok = mapToCanvas(e->position(), cx, cy);
		if (e->buttons() & Qt::LeftButton) {
			if (!ok)
				return;
			if (mode == Mode::Resize)
				resizeTo(cx, cy);
			else if (mode == Mode::Rotate)
				rotateTo(cx, cy,
					 (e->modifiers() & Qt::ControlModifier) != 0);
			else if (mode == Mode::Move && dragging)
				dragTo(cx, cy);
			return;
		}
		updateHoverCursor(e->position()); /* handle-aware cursor feedback */
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (e->button() == Qt::LeftButton)
			endInteraction();
	}

private:
	obs_display_t *display = nullptr;
	obs_weak_source_t *sceneWeak = nullptr;
	std::mutex mtx;

	/* interaction state (UI thread only) */
	enum class Mode { None, Move, Resize, Rotate };
	Mode mode = Mode::None;

	/* move-drag state */
	bool dragging = false;
	float grabX = 0, grabY = 0;
	float selL = 0, selT = 0, selR = 0, selB = 0; /* selection bbox at drag start */
	struct Held {
		obs_sceneitem_t *item;
		vec2 start;
	};
	std::vector<Held> held;

	/* resize / rotate target (a single item; addref'd for the interaction) */
	obs_sceneitem_t *xfItem = nullptr;
	int activeHandle = -1;
	uint32_t xfBoundsType = 0;
	vec2 startScale, startBounds, startPos;
	float startRot = 0;
	float ancX = 0, ancY = 0;     /* fixed anchor point (canvas) for resize */
	float uxX = 0, uxY = 0;       /* unit box x-axis (canvas) */
	float uyX = 0, uyY = 0;       /* unit box y-axis (canvas) */
	float span0X = 0, span0Y = 0; /* signed anchor->grabbed lengths at start */
	bool axX = false, axY = false;
	float cenX = 0, cenY = 0;     /* box center (canvas) for rotate */
	float rotGrabAngle = 0;

	/* snap targets, rebuilt at each interaction start: canvas edges/center plus
	   every other visible item's bbox edges/center. Written on the UI thread. */
	std::vector<float> snapVx, snapVy;

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

	/* canvas px -> device px (the inverse of mapToCanvas); used for handle grab
	   tests + cursor feedback so handles keep a constant on-screen size/reach */
	bool mapCanvasToDevice(float cx, float cy, float &dx, float &dy)
	{
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return false;
		dx = cx * scale + offX;
		dy = cy * scale + offY;
		return true;
	}

	/* transform a UV (unit-square) point through a box matrix -> canvas px */
	static void xf(const matrix4 &m, float u, float v, float &ox, float &oy)
	{
		vec3 p, r;
		vec3_set(&p, u, v, 0.0f);
		vec3_transform(&r, &p, &m);
		ox = r.x;
		oy = r.y;
	}

	/* ---- single-selection target (for resize/rotate) ---- */

	static bool selUnlockedCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) &&
		    !obs_sceneitem_locked(item)) {
			obs_sceneitem_addref(item);
			static_cast<std::vector<obs_sceneitem_t *> *>(param)->push_back(item);
		}
		return true;
	}

	/* the one selected, visible, unlocked item -- or null if zero or many.
	   Returns a ref the caller must release. */
	obs_sceneitem_t *singleSelected()
	{
		obs_source_t *sceneSrc = lockScene();
		if (!sceneSrc)
			return nullptr;
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		std::vector<obs_sceneitem_t *> v;
		if (scene)
			obs_scene_enum_items(scene, selUnlockedCb, &v);
		obs_source_release(sceneSrc);
		obs_sceneitem_t *one = nullptr;
		if (v.size() == 1) {
			one = v[0];
			obs_sceneitem_addref(one);
		}
		for (obs_sceneitem_t *it : v)
			obs_sceneitem_release(it);
		return one;
	}

	/* which handle (0..7), rotate stalk (-2), or none (-1) sits under a device
	   point for this item */
	int hitHandle(obs_sceneitem_t *item, float mdx, float mdy)
	{
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return -1;
		matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		for (int i = 0; i < 8; i++) {
			float hx, hy, dx, dy;
			xf(m, kHandles[i].gx, kHandles[i].gy, hx, hy);
			if (mapCanvasToDevice(hx, hy, dx, dy) &&
			    fabsf(dx - mdx) <= GRAB_PX && fabsf(dy - mdy) <= GRAB_PX)
				return i;
		}
		/* rotate stalk: off the top edge along the box's -y (outward) axis */
		float tcx, tcy, ox, oy, yx, yy;
		xf(m, 0.5f, 0.0f, tcx, tcy);
		xf(m, 0.0f, 0.0f, ox, oy);
		xf(m, 0.0f, 1.0f, yx, yy);
		float ax = yx - ox, ay = yy - oy;
		const float ln = sqrtf(ax * ax + ay * ay);
		if (ln > 1e-3f) {
			ax /= ln;
			ay /= ln;
		}
		const float rcx = tcx - ax * (ROT_OFFSET_PX / scale);
		const float rcy = tcy - ay * (ROT_OFFSET_PX / scale);
		float dx, dy;
		if (mapCanvasToDevice(rcx, rcy, dx, dy) && fabsf(dx - mdx) <= GRAB_PX &&
		    fabsf(dy - mdy) <= GRAB_PX)
			return -2;
		return -1;
	}

	void updateHoverCursor(const QPointF &pos)
	{
		const qreal dpr = devicePixelRatioF();
		const float mdx = (float)(pos.x() * dpr);
		const float mdy = (float)(pos.y() * dpr);
		int h = -1;
		obs_sceneitem_t *one = singleSelected();
		if (one) {
			h = hitHandle(one, mdx, mdy);
			obs_sceneitem_release(one);
		}
		if (h == -2)
			setCursor(Qt::PointingHandCursor);
		else if (h == 0 || h == 4)
			setCursor(Qt::SizeFDiagCursor);
		else if (h == 2 || h == 6)
			setCursor(Qt::SizeBDiagCursor);
		else if (h == 1 || h == 5)
			setCursor(Qt::SizeVerCursor);
		else if (h == 3 || h == 7)
			setCursor(Qt::SizeHorCursor);
		else
			unsetCursor();
	}

	/* ---- snap targets (canvas + other sources) ---- */

	struct SnapBuild {
		std::vector<float> *vx;
		std::vector<float> *vy;
	};
	static bool snapTargCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		/* the moving items are the selected ones; snap to everything else */
		if (obs_sceneitem_selected(item) || !obs_sceneitem_visible(item))
			return true;
		auto *sb = static_cast<SnapBuild *>(param);
		matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		float L = 1e30f, T = 1e30f, R = -1e30f, B = -1e30f;
		const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
		for (const auto &u : uv) {
			float x, y;
			xf(m, u[0], u[1], x, y);
			L = qMin(L, x);
			T = qMin(T, y);
			R = qMax(R, x);
			B = qMax(B, y);
		}
		sb->vx->push_back(L);
		sb->vx->push_back((L + R) * 0.5f);
		sb->vx->push_back(R);
		sb->vy->push_back(T);
		sb->vy->push_back((T + B) * 0.5f);
		sb->vy->push_back(B);
		return true;
	}

	void buildSnapTargets()
	{
		snapVx.clear();
		snapVy.clear();
		float scale, offX, offY;
		uint32_t cw, ch;
		if (!metrics(scale, offX, offY, cw, ch))
			return;
		const float W = (float)cw, H = (float)ch;
		snapVx.push_back(0.0f);
		snapVx.push_back(W * 0.5f);
		snapVx.push_back(W);
		snapVy.push_back(0.0f);
		snapVy.push_back(H * 0.5f);
		snapVy.push_back(H);
		obs_source_t *sceneSrc = lockScene();
		if (!sceneSrc)
			return;
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		SnapBuild sb{&snapVx, &snapVy};
		if (scene)
			obs_scene_enum_items(scene, snapTargCb, &sb);
		obs_source_release(sceneSrc);
	}

	/* nudge a single value onto the nearest snap target within radius */
	bool snap1D(float value, const std::vector<float> &targets, float &adj,
		    float &gpos)
	{
		const float rad = snapRadius();
		if (rad <= 0.0f)
			return false;
		float best = rad;
		bool got = false;
		for (float t : targets) {
			const float nd = t - value;
			if (fabsf(nd) < best) {
				best = fabsf(nd);
				adj = nd;
				gpos = t;
				got = true;
			}
		}
		return got;
	}

	/* ---- resize ---- */

	void beginResize(obs_sceneitem_t *item, int h)
	{
		endInteraction();
		matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		float ox, oy, xx, xy, yx, yy;
		xf(m, 0, 0, ox, oy);
		xf(m, 1, 0, xx, xy);
		xf(m, 0, 1, yx, yy);
		const float Xx = xx - ox, Xy = xy - oy;
		const float Yx = yx - ox, Yy = yy - oy;
		const float lx = sqrtf(Xx * Xx + Xy * Xy);
		const float ly = sqrtf(Yx * Yx + Yy * Yy);
		if (lx < 1e-3f || ly < 1e-3f)
			return; /* degenerate box; leave selection as-is */
		xfItem = item;
		obs_sceneitem_addref(xfItem);
		mode = Mode::Resize;
		activeHandle = h;
		xfBoundsType = (uint32_t)obs_sceneitem_get_bounds_type(item);
		obs_sceneitem_get_scale(item, &startScale);
		obs_sceneitem_get_bounds(item, &startBounds);
		obs_sceneitem_get_pos(item, &startPos);
		uxX = Xx / lx;
		uxY = Xy / lx;
		uyX = Yx / ly;
		uyY = Yy / ly;
		xf(m, kHandles[h].ax, kHandles[h].ay, ancX, ancY);
		float gx, gy;
		xf(m, kHandles[h].gx, kHandles[h].gy, gx, gy);
		span0X = (gx - ancX) * uxX + (gy - ancY) * uxY;
		span0Y = (gx - ancX) * uyX + (gy - ancY) * uyY;
		axX = kHandles[h].sx;
		axY = kHandles[h].sy;
		buildSnapTargets();
	}

	void resizeTo(float cx, float cy)
	{
		if (!xfItem)
			return;
		const float dX = cx - ancX, dY = cy - ancY;
		float spanX = axX ? (dX * uxX + dY * uxY) : span0X;
		float spanY = axY ? (dX * uyX + dY * uyY) : span0Y;
		/* predicted grabbed corner (canvas), then snap its x/y to targets */
		float gpx = ancX + uxX * spanX + uyX * spanY;
		float gpy = ancY + uxY * spanX + uyY * spanY;
		snapX = snapY = false;
		if (axX) {
			float adj, gpos;
			if (snap1D(gpx, snapVx, adj, gpos)) {
				gpx += adj;
				snapX = true;
				snapXpos = gpos;
			}
		}
		if (axY) {
			float adj, gpos;
			if (snap1D(gpy, snapVy, adj, gpos)) {
				gpy += adj;
				snapY = true;
				snapYpos = gpos;
			}
		}
		spanX = (gpx - ancX) * uxX + (gpy - ancY) * uxY;
		spanY = (gpx - ancX) * uyX + (gpy - ancY) * uyY;
		float rX = axX ? spanX / span0X : 1.0f;
		float rY = axY ? spanY / span0Y : 1.0f;
		const float minR = 0.02f; /* never zero/flip the item */
		if (rX < minR)
			rX = minR;
		if (rY < minR)
			rY = minR;
		if (xfBoundsType != (uint32_t)OBS_BOUNDS_NONE) {
			vec2 b;
			b.x = startBounds.x * rX;
			b.y = startBounds.y * rY;
			obs_sceneitem_set_bounds(xfItem, &b);
		} else {
			vec2 sc;
			sc.x = startScale.x * rX;
			sc.y = startScale.y * rY;
			obs_sceneitem_set_scale(xfItem, &sc);
		}
		/* re-anchor: translate pos so the fixed corner returns to where it was
		   (a pos shift moves the whole box rigidly, per dockx-align.cpp) */
		matrix4 m2;
		obs_sceneitem_get_box_transform(xfItem, &m2);
		float a2x, a2y;
		xf(m2, kHandles[activeHandle].ax, kHandles[activeHandle].ay, a2x, a2y);
		vec2 pos;
		obs_sceneitem_get_pos(xfItem, &pos);
		pos.x += (ancX - a2x);
		pos.y += (ancY - a2y);
		obs_sceneitem_set_pos(xfItem, &pos);
	}

	/* ---- rotate ---- */

	void beginRotate(obs_sceneitem_t *item, float cx, float cy)
	{
		endInteraction();
		xfItem = item;
		obs_sceneitem_addref(xfItem);
		mode = Mode::Rotate;
		obs_sceneitem_get_pos(item, &startPos);
		startRot = obs_sceneitem_get_rot(item);
		matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		xf(m, 0.5f, 0.5f, cenX, cenY);
		rotGrabAngle = atan2f(cy - cenY, cx - cenX);
	}

	void rotateTo(float cx, float cy, bool snap15)
	{
		if (!xfItem)
			return;
		const float ang = atan2f(cy - cenY, cx - cenX);
		float deg = startRot + (ang - rotGrabAngle) * (180.0f / PI_F);
		if (snap15)
			deg = roundf(deg / 15.0f) * 15.0f;
		while (deg >= 360.0f)
			deg -= 360.0f;
		while (deg < 0.0f)
			deg += 360.0f;
		obs_sceneitem_set_rot(xfItem, deg);
		/* re-anchor: keep the visual center fixed while spinning */
		matrix4 m2;
		obs_sceneitem_get_box_transform(xfItem, &m2);
		float c2x, c2y;
		xf(m2, 0.5f, 0.5f, c2x, c2y);
		vec2 pos;
		obs_sceneitem_get_pos(xfItem, &pos);
		pos.x += (cenX - c2x);
		pos.y += (cenY - c2y);
		obs_sceneitem_set_pos(xfItem, &pos);
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
		endInteraction();
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
		mode = dragging ? Mode::Move : Mode::None;
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
		buildSnapTargets();
	}

	/* nudge dx/dy so the selection bbox latches onto a snap target (a canvas
	   edge/center or any other source's edge/center) */
	void computeSnap(float &dx, float &dy)
	{
		snapX = snapY = false;
		if (held.empty())
			return;
		const float rad = snapRadius();
		if (rad <= 0.0f)
			return;
		{
			const float ex[3] = {selL + dx, selR + dx, (selL + selR) * 0.5f + dx};
			float best = rad, adj = 0, gpos = 0;
			bool got = false;
			for (float c : ex)
				for (float t : snapVx) {
					const float nd = t - c;
					if (fabsf(nd) < best) {
						best = fabsf(nd);
						adj = nd;
						gpos = t;
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
			float best = rad, adj = 0, gpos = 0;
			bool got = false;
			for (float c : ey)
				for (float t : snapVy) {
					const float nd = t - c;
					if (fabsf(nd) < best) {
						best = fabsf(nd);
						adj = nd;
						gpos = t;
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

	/* end any interaction (move / resize / rotate); releases all held refs */
	void endInteraction()
	{
		for (Held &h : held)
			obs_sceneitem_release(h.item);
		held.clear();
		if (xfItem) {
			obs_sceneitem_release(xfItem);
			xfItem = nullptr;
		}
		mode = Mode::None;
		dragging = false;
		activeHandle = -1;
		snapX = snapY = false;
		snapVx.clear();
		snapVy.clear();
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

	/* a screen-axis-aligned filled rectangle (canvas coords map straight to the
	   viewport, so a canvas-aligned quad is screen-aligned) */
	static void drawFilledRect(float x0, float y0, float x1, float y1, uint32_t rgba)
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
			gs_vertex2f(x0, y0);
			gs_vertex2f(x1, y0);
			gs_vertex2f(x0, y1);
			gs_vertex2f(x1, y1);
			gs_render_stop(GS_TRISTRIP);
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

	/* boxes of selected + visible + UNLOCKED items -- handles show only when
	   there is exactly one (single-item resize/rotate) */
	static bool collectHandleBoxes(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) &&
		    !obs_sceneitem_locked(item)) {
			auto *v = static_cast<std::vector<matrix4> *>(param);
			matrix4 m;
			obs_sceneitem_get_box_transform(item, &m);
			v->push_back(m);
		}
		return true;
	}

	/* the 8 resize squares + the rotate stalk, at a constant on-screen size */
	void drawHandles(obs_scene_t *scene, float scale)
	{
		if (scale <= 0.0f)
			return;
		std::vector<matrix4> boxes;
		obs_scene_enum_items(scene, collectHandleBoxes, &boxes);
		if (boxes.size() != 1)
			return;
		const matrix4 &m = boxes[0];
		const float half = HANDLE_HALF_PX / scale;
		for (int i = 0; i < 8; i++) {
			float hx, hy;
			xf(m, kHandles[i].gx, kHandles[i].gy, hx, hy);
			drawFilledRect(hx - half, hy - half, hx + half, hy + half,
				       COLOR_SELECT);
		}
		/* rotate stalk off the top edge along the box's outward (-y) axis */
		float tcx, tcy, ox, oy, yx, yy;
		xf(m, 0.5f, 0.0f, tcx, tcy);
		xf(m, 0.0f, 0.0f, ox, oy);
		xf(m, 0.0f, 1.0f, yx, yy);
		float ax = yx - ox, ay = yy - oy;
		const float ln = sqrtf(ax * ax + ay * ay);
		if (ln > 1e-3f) {
			ax /= ln;
			ay /= ln;
		}
		const float rx = tcx - ax * (ROT_OFFSET_PX / scale);
		const float ry = tcy - ay * (ROT_OFFSET_PX / scale);
		std::vector<vec2> stalk{mk(tcx, tcy), mk(rx, ry)};
		drawLineStrip(stalk, COLOR_SELECT);
		drawFilledRect(rx - half, ry - half, rx + half, ry + half, COLOR_SELECT);
	}

	void drawOverlay(obs_source_t *sceneSrc, uint32_t w, uint32_t h, float scale)
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
		if (scene)
			drawHandles(scene, scale);
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
		self->drawOverlay(scene, w, h, scale);
		gs_projection_pop();
		gs_viewport_pop();
		obs_source_release(scene);
	}
};

/* the dock content: a thin control bar (the ONE always-visible switch back to
   OBS's built-in preview) above the editable video surface */
class EditPreviewPanel : public QWidget {
public:
	int id;
	EditWidget *video;

	EditPreviewPanel(int id_) : QWidget(nullptr), id(id_)
	{
		QVBoxLayout *v = new QVBoxLayout(this);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(0);

		QWidget *bar = new QWidget(this);
		QHBoxLayout *h = new QHBoxLayout(bar);
		h->setContentsMargins(8, 4, 8, 4);
		h->setSpacing(8);
		QLabel *tag = new QLabel("DockX Preview", bar);
		h->addWidget(tag);
		h->addStretch(1);
		obsBtn = new QPushButton(bar);
		obsBtn->setToolTip("Show or hide OBS's built-in preview (the fixed "
				   "one in the middle of the window)");
		h->addWidget(obsBtn);
		v->addWidget(bar);

		video = new EditWidget(id_, this);
		v->addWidget(video, 1);

		/* one control, both directions: this dock IS the movable, editable
		   preview, so its button just governs whether OBS's fixed built-in
		   preview is shown or hidden -- the always-there way back */
		QObject::connect(obsBtn, &QPushButton::clicked, this, []() {
			preview::setCollapsed(!preview::collapsed());
		});
		updateButton();
	}

	void refresh()
	{
		video->refresh();
		updateButton();
	}

	void teardown() { video->teardown(); }

	void updateButton()
	{
		obsBtn->setText(preview::collapsed() ? "Show OBS preview"
						     : "Hide OBS preview");
	}

private:
	QPushButton *obsBtn;
};

static std::vector<EditPreviewPanel *> g_panels;

static EditPreviewPanel *registerDock(int id)
{
	EditPreviewPanel *p = new EditPreviewPanel(id);
	if (!obs_frontend_add_dock_by_id(dockIdFor(id).toUtf8().constData(),
					 "DockX Preview", p)) {
		obs_log(LOG_WARNING, "could not register DockX Preview dock %d", id);
		delete p;
		return nullptr;
	}
	g_panels.push_back(p);
	return p;
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
	for (EditPreviewPanel *p : g_panels)
		p->refresh();
}

void addDock()
{
	const int id = state().nextEditDockId++;
	state().editDocks.push_back(id);
	stateSave();
	EditPreviewPanel *p = registerDock(id);
	if (!p)
		return;
	p->refresh();
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
	for (auto it = g_panels.begin(); it != g_panels.end(); ++it) {
		if ((*it)->id == id) {
			(*it)->teardown();
			g_panels.erase(it);
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
	for (EditPreviewPanel *p : g_panels)
		p->teardown();
	g_panels.clear();
}

} // namespace editpreview
} // namespace dockx
