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
	{0.0f, 0.0f, 1.0f, 1.0f, true, true},  /* TL */
	{0.5f, 0.0f, 0.5f, 1.0f, false, true}, /* TC */
	{1.0f, 0.0f, 0.0f, 1.0f, true, true},  /* TR */
	{1.0f, 0.5f, 0.0f, 0.5f, true, false}, /* MR */
	{1.0f, 1.0f, 0.0f, 0.0f, true, true},  /* BR */
	{0.5f, 1.0f, 0.5f, 0.0f, false, true}, /* BC */
	{0.0f, 1.0f, 1.0f, 0.0f, true, true},  /* BL */
	{0.0f, 0.5f, 1.0f, 0.5f, true, false}, /* ML */
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

	/* UI thread: point the dock at the scene the user is editing. In studio
	   mode that is the PREVIEW (staging) scene -- edits land there and only go
	   live on transition, matching OBS's studio workflow -- otherwise it is the
	   current program scene. Re-resolved on every scene / preview-scene /
	   studio-mode event via refreshAll(). */
	void refresh()
	{
		obs_source_t *scene = obs_frontend_preview_program_mode_active()
					      ? obs_frontend_get_current_preview_scene()
					      : obs_frontend_get_current_scene();
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
			obs_display_resize(display, (uint32_t)(width() * dpr), (uint32_t)(height() * dpr));
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
		const bool alt = (e->modifiers() & Qt::AltModifier) != 0;
		const qreal dpr = devicePixelRatioF();
		const float mdx = (float)(e->position().x() * dpr);
		const float mdy = (float)(e->position().y() * dpr);
		/* a transform handle on a single selected, unlocked item wins first:
		   the corner/edge squares resize it (Alt + an edge crops instead), the
		   stalk above it rotates it. */
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
					/* Alt + handle = crop: an edge crops that side, a
					   corner crops the two adjacent sides. Falls back to
					   resize if the item can't be cleanly cropped. */
					if (alt) {
						beginCrop(one, h);
						if (mode == Mode::Crop) {
							obs_sceneitem_release(one);
							setFocus();
							return;
						}
					}
					beginResize(one, h);
					obs_sceneitem_release(one);
					setFocus();
					return;
				}
				obs_sceneitem_release(one);
			} else {
				/* 2+ selected: handles wrap the whole group's bbox */
				float L, T, R, B;
				int cnt;
				if (groupAABB(L, T, R, B, cnt) && cnt >= 2) {
					const int h = hitHandleGroup(L, T, R, B, mdx, mdy);
					if (h == -2) {
						beginGroupRotate(L, T, R, B, cx, cy);
						setFocus();
						return;
					}
					if (h >= 0) {
						beginGroupResize(h, L, T, R, B);
						setFocus();
						return;
					}
				}
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
			else if (mode == Mode::Crop)
				cropTo(cx, cy);
			else if (mode == Mode::GroupResize)
				groupResizeTo(cx, cy);
			else if (mode == Mode::GroupRotate)
				groupRotateTo(cx, cy, (e->modifiers() & Qt::ControlModifier) != 0);
			else if (mode == Mode::Rotate)
				rotateTo(cx, cy, (e->modifiers() & Qt::ControlModifier) != 0);
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
	enum class Mode { None, Move, Resize, Rotate, GroupResize, GroupRotate, Crop };
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
	float cenX = 0, cenY = 0; /* box center (canvas) for rotate */
	float rotGrabAngle = 0;

	/* group resize (2+ items): the handles wrap the axis-aligned group bbox;
	   each item scales about the shared anchor. Per-item start capture below. */
	struct GItem {
		obs_sceneitem_t *item;
		uint32_t boundsType;
		vec2 scale, bounds, pos;
		float refX, refY;       /* the item's box (0,0) corner at start (canvas) */
		float startRot;         /* item rotation at start (group rotate) */
		float startCX, startCY; /* item box center at start (group rotate) */
	};
	std::vector<GItem> groupItems;

	/* crop-drag (Alt + drag an edge or corner handle) on a single item, at any
	   rotation. Crop is measured in SOURCE px along the item's own axes, so
	   canvas movement is projected onto those axes and mapped through the
	   source->canvas scale; the opposite edge/corner is re-anchored. Corners
	   crop the two adjacent sides at once. Bounds-fitted items crop too, but
	   their box is pinned by the bounds so we skip the re-anchor for them
	   (OBS does the same) -- the content just refits inside the fixed box. */
	bool cropL = false, cropT = false, cropR = false, cropB = false;
	bool cropIsBounds = false; /* bounds-fit item: crop but don't reposition */
	obs_sceneitem_crop startCrop = {};
	uint32_t srcW = 0, srcH = 0;
	float cropScaleX = 0, cropScaleY = 0; /* canvas px per source px (local axes) */
	float cropUxX = 0, cropUxY = 0;       /* item unit x-axis (canvas) */
	float cropUyX = 0, cropUyY = 0;       /* item unit y-axis (canvas) */
	float cropAncX = 0, cropAncY = 0;     /* opposite edge/corner anchor (canvas) */
	float cropStartX = 0, cropStartY = 0; /* dragged handle start (canvas) */

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
		if (!wireDisplayWindow(info, (quintptr)winId())) {
			obs_log(LOG_WARNING,
				"edit dock %d: display unsupported on this platform (Wayland needs Qt 6.9+)", id);
			return;
		}
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
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) && !obs_sceneitem_locked(item)) {
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
			if (mapCanvasToDevice(hx, hy, dx, dy) && fabsf(dx - mdx) <= GRAB_PX &&
			    fabsf(dy - mdy) <= GRAB_PX)
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
		if (mapCanvasToDevice(rcx, rcy, dx, dy) && fabsf(dx - mdx) <= GRAB_PX && fabsf(dy - mdy) <= GRAB_PX)
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
		} else {
			float L, T, R, B;
			int cnt;
			if (groupAABB(L, T, R, B, cnt) && cnt >= 2)
				h = hitHandleGroup(L, T, R, B, mdx, mdy);
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
	bool snap1D(float value, const std::vector<float> &targets, float &adj, float &gpos)
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

	/* ---- group resize (2+ items) ---- */

	/* axis-aligned bounding box of all selected, visible, unlocked items */
	struct GAABB {
		float L, T, R, B;
		int count;
	};
	static bool gaabbCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) && !obs_sceneitem_locked(item)) {
			auto *g = static_cast<GAABB *>(param);
			matrix4 m;
			obs_sceneitem_get_box_transform(item, &m);
			const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (const auto &u : uv) {
				float x, y;
				xf(m, u[0], u[1], x, y);
				g->L = qMin(g->L, x);
				g->T = qMin(g->T, y);
				g->R = qMax(g->R, x);
				g->B = qMax(g->B, y);
			}
			g->count++;
		}
		return true;
	}
	bool groupAABB(float &L, float &T, float &R, float &B, int &count)
	{
		obs_source_t *sceneSrc = lockScene();
		if (!sceneSrc) {
			count = 0;
			return false;
		}
		obs_scene_t *scene = obs_scene_from_source(sceneSrc);
		GAABB g{1e30f, 1e30f, -1e30f, -1e30f, 0};
		if (scene)
			obs_scene_enum_items(scene, gaabbCb, &g);
		obs_source_release(sceneSrc);
		L = g.L;
		T = g.T;
		R = g.R;
		B = g.B;
		count = g.count;
		return count > 0;
	}

	/* handle (0..7) on the axis-aligned group bbox under a device point, the
	   rotate stalk (-2) above its top-center, else -1 */
	int hitHandleGroup(float L, float T, float R, float B, float mdx, float mdy)
	{
		for (int i = 0; i < 8; i++) {
			const float hx = L + (R - L) * kHandles[i].gx;
			const float hy = T + (B - T) * kHandles[i].gy;
			float dx, dy;
			if (mapCanvasToDevice(hx, hy, dx, dy) && fabsf(dx - mdx) <= GRAB_PX &&
			    fabsf(dy - mdy) <= GRAB_PX)
				return i;
		}
		float scale, offX, offY;
		uint32_t cw, ch;
		if (metrics(scale, offX, offY, cw, ch)) {
			const float rx = (L + R) * 0.5f;
			const float ry = T - (ROT_OFFSET_PX / scale);
			float dx, dy;
			if (mapCanvasToDevice(rx, ry, dx, dy) && fabsf(dx - mdx) <= GRAB_PX &&
			    fabsf(dy - mdy) <= GRAB_PX)
				return -2;
		}
		return -1;
	}

	static bool gcaptureCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
	{
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) && !obs_sceneitem_locked(item)) {
			auto *v = static_cast<std::vector<GItem> *>(param);
			obs_sceneitem_addref(item);
			GItem gi;
			gi.item = item;
			gi.boundsType = (uint32_t)obs_sceneitem_get_bounds_type(item);
			obs_sceneitem_get_scale(item, &gi.scale);
			obs_sceneitem_get_bounds(item, &gi.bounds);
			obs_sceneitem_get_pos(item, &gi.pos);
			gi.startRot = obs_sceneitem_get_rot(item);
			matrix4 m;
			obs_sceneitem_get_box_transform(item, &m);
			xf(m, 0, 0, gi.refX, gi.refY);
			xf(m, 0.5f, 0.5f, gi.startCX, gi.startCY);
			v->push_back(gi);
		}
		return true;
	}

	void beginGroupResize(int h, float L, float T, float R, float B)
	{
		endInteraction();
		if (R - L < 1e-2f || B - T < 1e-2f)
			return; /* degenerate group */
		activeHandle = h;
		uxX = 1;
		uxY = 0;
		uyX = 0;
		uyY = 1; /* the group bbox is axis-aligned */
		ancX = L + (R - L) * kHandles[h].ax;
		ancY = T + (B - T) * kHandles[h].ay;
		span0X = (L + (R - L) * kHandles[h].gx) - ancX;
		span0Y = (T + (B - T) * kHandles[h].gy) - ancY;
		axX = kHandles[h].sx;
		axY = kHandles[h].sy;
		obs_source_t *sceneSrc = lockScene();
		if (sceneSrc) {
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene)
				obs_scene_enum_items(scene, gcaptureCb, &groupItems);
			obs_source_release(sceneSrc);
		}
		if (groupItems.empty())
			return;
		mode = Mode::GroupResize;
		buildSnapTargets();
	}

	void groupResizeTo(float cx, float cy)
	{
		if (groupItems.empty())
			return;
		float spanX = axX ? (cx - ancX) : span0X;
		float spanY = axY ? (cy - ancY) : span0Y;
		float gpx = ancX + spanX; /* group axes are canvas x/y */
		float gpy = ancY + spanY;
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
		float rX = axX ? (gpx - ancX) / span0X : 1.0f;
		float rY = axY ? (gpy - ancY) / span0Y : 1.0f;
		const float minR = 0.02f;
		if (rX < minR)
			rX = minR;
		if (rY < minR)
			rY = minR;
		for (GItem &gi : groupItems) {
			if (gi.boundsType != (uint32_t)OBS_BOUNDS_NONE) {
				vec2 b;
				b.x = gi.bounds.x * rX;
				b.y = gi.bounds.y * rY;
				obs_sceneitem_set_bounds(gi.item, &b);
			} else {
				vec2 sc;
				sc.x = gi.scale.x * rX;
				sc.y = gi.scale.y * rY;
				obs_sceneitem_set_scale(gi.item, &sc);
			}
			/* each item's top-left corner maps to the group-scaled spot, then
			   re-anchor it there (a pos shift moves the box rigidly) */
			const float desX = ancX + (gi.refX - ancX) * rX;
			const float desY = ancY + (gi.refY - ancY) * rY;
			matrix4 m2;
			obs_sceneitem_get_box_transform(gi.item, &m2);
			float a0x, a0y;
			xf(m2, 0, 0, a0x, a0y);
			vec2 pos;
			obs_sceneitem_get_pos(gi.item, &pos);
			pos.x += (desX - a0x);
			pos.y += (desY - a0y);
			obs_sceneitem_set_pos(gi.item, &pos);
		}
	}

	/* spin every selected item about the group center: each item's own rotation
	   advances by the drag angle AND its center orbits the group center */
	void beginGroupRotate(float L, float T, float R, float B, float cx, float cy)
	{
		endInteraction();
		cenX = (L + R) * 0.5f;
		cenY = (T + B) * 0.5f;
		rotGrabAngle = atan2f(cy - cenY, cx - cenX);
		obs_source_t *sceneSrc = lockScene();
		if (sceneSrc) {
			obs_scene_t *scene = obs_scene_from_source(sceneSrc);
			if (scene)
				obs_scene_enum_items(scene, gcaptureCb, &groupItems);
			obs_source_release(sceneSrc);
		}
		if (groupItems.empty())
			return;
		mode = Mode::GroupRotate;
	}

	/* soft angular assist for FREE (non-Ctrl) rotation: if the angle lands
	   within `thr` deg of a multiple of `step`, click it there. Lets a drag
	   settle on clean angles without forcing a grid the way Ctrl's hard 15-deg
	   snap does. Mirrors OBS's own no-modifier rotate snapping. */
	static float softSnapAngle(float a, float step, float thr)
	{
		const float n = roundf(a / step) * step;
		return (fabsf(a - n) <= thr) ? n : a;
	}

	void groupRotateTo(float cx, float cy, bool snap15)
	{
		if (groupItems.empty())
			return;
		const float ang = atan2f(cy - cenY, cx - cenX);
		float dDeg = (ang - rotGrabAngle) * (180.0f / PI_F);
		if (snap15)
			dDeg = roundf(dDeg / 15.0f) * 15.0f;
		else
			dDeg = softSnapAngle(dDeg, 15.0f, 5.0f);
		const float rad = dDeg * (PI_F / 180.0f);
		const float cs = cosf(rad), sn = sinf(rad);
		for (GItem &gi : groupItems) {
			const float ox = gi.startCX - cenX, oy = gi.startCY - cenY;
			const float nx = cenX + (ox * cs - oy * sn);
			const float ny = cenY + (ox * sn + oy * cs);
			float nr = gi.startRot + dDeg;
			while (nr >= 360.0f)
				nr -= 360.0f;
			while (nr < 0.0f)
				nr += 360.0f;
			obs_sceneitem_set_rot(gi.item, nr);
			matrix4 m2;
			obs_sceneitem_get_box_transform(gi.item, &m2);
			float c2x, c2y;
			xf(m2, 0.5f, 0.5f, c2x, c2y);
			vec2 pos;
			obs_sceneitem_get_pos(gi.item, &pos);
			pos.x += (nx - c2x);
			pos.y += (ny - c2y);
			obs_sceneitem_set_pos(gi.item, &pos);
		}
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
		else {
			/* free rotate still softly clicks to clean angles and back
			   to the item's original rotation (a "reset tilt" magnet) */
			deg = softSnapAngle(deg, 15.0f, 5.0f);
			if (fabsf(deg - startRot) <= 5.0f)
				deg = startRot;
		}
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

	/* ---- crop-drag (Alt + edge handle) ---- */

	/* sets up a crop drag; leaves mode == None (a no-op) if the item is already
	   fully cropped or degenerate -- the caller then does a resize. Works at any
	   rotation (crop runs on the item's own axes) and for bounds-fitted items
	   (their box is pinned, so we crop without repositioning -- see cropTo). h
	   is any of the 8 handles: edges crop one side, corners crop two. */
	void beginCrop(obs_sceneitem_t *item, int h)
	{
		endInteraction();
		/* bounds-fit items crop too, but the crop refits the content inside a
		   pinned box, so we must NOT re-anchor the position afterward (OBS
		   guards its set_pos on OBS_BOUNDS_NONE the same way). */
		const bool boundsFit = obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE;
		obs_source_t *src = obs_sceneitem_get_source(item); /* borrowed */
		if (!src)
			return;
		srcW = obs_source_get_width(src);
		srcH = obs_source_get_height(src);
		if (!srcW || !srcH)
			return;
		obs_sceneitem_get_crop(item, &startCrop);
		const int visW = (int)srcW - startCrop.left - startCrop.right;
		const int visH = (int)srcH - startCrop.top - startCrop.bottom;
		if (visW <= 0 || visH <= 0)
			return;
		matrix4 m;
		obs_sceneitem_get_box_transform(item, &m);
		float ox, oy, xx, xy, yx, yy;
		xf(m, 0, 0, ox, oy);
		xf(m, 1, 0, xx, xy);
		xf(m, 0, 1, yx, yy);
		const float lx = sqrtf((xx - ox) * (xx - ox) + (xy - oy) * (xy - oy));
		const float ly = sqrtf((yx - ox) * (yx - ox) + (yy - oy) * (yy - oy));
		if (lx < 1e-3f || ly < 1e-3f)
			return;
		cropUxX = (xx - ox) / lx;
		cropUxY = (xy - oy) / lx;
		cropUyX = (yx - ox) / ly;
		cropUyY = (yy - oy) / ly;
		cropScaleX = lx / (float)visW; /* canvas px per source px, local x */
		cropScaleY = ly / (float)visH;
		if (cropScaleX <= 0.0f || cropScaleY <= 0.0f)
			return;
		/* which sides this handle crops */
		cropL = cropT = cropR = cropB = false;
		switch (h) {
		case 7:
			cropL = true;
			break;
		case 3:
			cropR = true;
			break;
		case 1:
			cropT = true;
			break;
		case 5:
			cropB = true;
			break;
		case 0:
			cropL = cropT = true;
			break;
		case 2:
			cropR = cropT = true;
			break;
		case 4:
			cropR = cropB = true;
			break;
		case 6:
			cropL = cropB = true;
			break;
		default:
			return;
		}
		activeHandle = h;
		xf(m, kHandles[h].ax, kHandles[h].ay, cropAncX, cropAncY);
		xf(m, kHandles[h].gx, kHandles[h].gy, cropStartX, cropStartY);
		xfItem = item;
		obs_sceneitem_addref(xfItem);
		cropIsBounds = boundsFit;
		mode = Mode::Crop;
	}

	void cropTo(float cx, float cy)
	{
		if (!xfItem || mode != Mode::Crop)
			return;
		const float mvx = cx - cropStartX, mvy = cy - cropStartY;
		const float alongX = mvx * cropUxX + mvy * cropUxY; /* +x = local right */
		const float alongY = mvx * cropUyX + mvy * cropUyY; /* +y = local down */
		obs_sceneitem_crop c = startCrop;
		if (cropR) { /* right edge inward = -local x */
			c.right = startCrop.right + (int)roundf(-alongX / cropScaleX);
			if (c.right < 0)
				c.right = 0;
			if (c.right > (int)srcW - startCrop.left - 1)
				c.right = (int)srcW - startCrop.left - 1;
		}
		if (cropL) { /* left edge inward = +local x */
			c.left = startCrop.left + (int)roundf(alongX / cropScaleX);
			if (c.left < 0)
				c.left = 0;
			if (c.left > (int)srcW - startCrop.right - 1)
				c.left = (int)srcW - startCrop.right - 1;
		}
		if (cropB) { /* bottom edge inward = -local y */
			c.bottom = startCrop.bottom + (int)roundf(-alongY / cropScaleY);
			if (c.bottom < 0)
				c.bottom = 0;
			if (c.bottom > (int)srcH - startCrop.top - 1)
				c.bottom = (int)srcH - startCrop.top - 1;
		}
		if (cropT) { /* top edge inward = +local y */
			c.top = startCrop.top + (int)roundf(alongY / cropScaleY);
			if (c.top < 0)
				c.top = 0;
			if (c.top > (int)srcH - startCrop.bottom - 1)
				c.top = (int)srcH - startCrop.bottom - 1;
		}
		obs_sceneitem_set_crop(xfItem, &c);
		/* re-anchor: keep the opposite (un-cropped) edge/corner visually fixed.
		   Skipped for bounds-fit items: their box is pinned by the bounds, so
		   the far edge never moves and a pos shift would drag the whole box
		   (OBS likewise only repositions when OBS_BOUNDS_NONE). */
		if (cropIsBounds)
			return;
		matrix4 m2;
		obs_sceneitem_get_box_transform(xfItem, &m2);
		float a2x, a2y;
		xf(m2, kHandles[activeHandle].ax, kHandles[activeHandle].ay, a2x, a2y);
		vec2 pos;
		obs_sceneitem_get_pos(xfItem, &pos);
		pos.x += (cropAncX - a2x);
		pos.y += (cropAncY - a2y);
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
		if (!s->hit && obs_sceneitem_selected(item) && obs_sceneitem_visible(item) &&
		    !obs_sceneitem_locked(item) && pointInItem(item, s->cx, s->cy))
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
			if (!found && !obs_sceneitem_locked(item) && obs_sceneitem_visible(item) &&
			    pointInItem(item, cx, cy))
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
		for (GItem &gi : groupItems)
			obs_sceneitem_release(gi.item);
		groupItems.clear();
		if (xfItem) {
			obs_sceneitem_release(xfItem);
			xfItem = nullptr;
		}
		mode = Mode::None;
		dragging = false;
		activeHandle = -1;
		cropL = cropT = cropR = cropB = false;
		cropIsBounds = false;
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
		if (obs_sceneitem_selected(item) && obs_sceneitem_visible(item) && !obs_sceneitem_locked(item)) {
			auto *v = static_cast<std::vector<matrix4> *>(param);
			matrix4 m;
			obs_sceneitem_get_box_transform(item, &m);
			v->push_back(m);
		}
		return true;
	}

	/* the 8 resize squares + the rotate stalk, at a constant on-screen size.
	   One item -> handles on its (possibly rotated) box + a rotate stalk;
	   two or more -> handles on the axis-aligned group bbox (no rotate). */
	void drawHandles(obs_scene_t *scene, float scale)
	{
		if (scale <= 0.0f)
			return;
		std::vector<matrix4> boxes;
		obs_scene_enum_items(scene, collectHandleBoxes, &boxes);
		const float half = HANDLE_HALF_PX / scale;
		if (boxes.size() >= 2) {
			float L = 1e30f, T = 1e30f, R = -1e30f, B = -1e30f;
			const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (const matrix4 &m : boxes)
				for (const auto &u : uv) {
					float x, y;
					xf(m, u[0], u[1], x, y);
					L = qMin(L, x);
					T = qMin(T, y);
					R = qMax(R, x);
					B = qMax(B, y);
				}
			std::vector<vec2> outline{mk(L, T), mk(R, T), mk(R, B), mk(L, B), mk(L, T)};
			drawLineStrip(outline, COLOR_SELECT);
			for (int i = 0; i < 8; i++) {
				const float hx = L + (R - L) * kHandles[i].gx;
				const float hy = T + (B - T) * kHandles[i].gy;
				drawFilledRect(hx - half, hy - half, hx + half, hy + half, COLOR_SELECT);
			}
			/* rotate stalk above the group's top-center */
			const float grx = (L + R) * 0.5f;
			const float gry = T - (ROT_OFFSET_PX / scale);
			std::vector<vec2> gstalk{mk(grx, T), mk(grx, gry)};
			drawLineStrip(gstalk, COLOR_SELECT);
			drawFilledRect(grx - half, gry - half, grx + half, gry + half, COLOR_SELECT);
			return;
		}
		if (boxes.size() != 1)
			return;
		const matrix4 &m = boxes[0];
		for (int i = 0; i < 8; i++) {
			float hx, hy;
			xf(m, kHandles[i].gx, kHandles[i].gy, hx, hy);
			drawFilledRect(hx - half, hy - half, hx + half, hy + half, COLOR_SELECT);
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
		QObject::connect(obsBtn, &QPushButton::clicked, this,
				 []() { preview::setCollapsed(!preview::collapsed()); });
		updateButton();
	}

	void refresh()
	{
		video->refresh();
		updateButton();
	}

	void teardown() { video->teardown(); }

	void updateButton() { obsBtn->setText(preview::collapsed() ? "Show OBS preview" : "Hide OBS preview"); }

private:
	QPushButton *obsBtn;
};

static std::vector<EditPreviewPanel *> g_panels;

static EditPreviewPanel *registerDock(int id)
{
	EditPreviewPanel *p = new EditPreviewPanel(id);
	if (!obs_frontend_add_dock_by_id(dockIdFor(id).toUtf8().constData(), "DockX Preview", p)) {
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
