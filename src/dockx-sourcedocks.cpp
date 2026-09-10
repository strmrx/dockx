/*
DockX for OBS Studio (by StrmrX) -- live source docks (phase 2).
GPL v2, see plugin-main.cpp for the full notice.

Renders any source, scene, the Preview (studio mode) or the Program inside a
regular OBS dock via an obs_display bound to a native Qt widget. Sources are
held as weak references and resolved by name on the UI thread; the draw
callback (graphics thread) only touches libobs.

Phase 2: audio sources get a volume slider + mute button under the video
(audio only sources are just the control strip), and interaction capable
sources (browser sources) receive mouse/keyboard input straight through the
dock, like OBS's own Interact window.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QContextMenuEvent>
#include <QDockWidget>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMenu>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <atomic>
#include <cmath>
#include <mutex>
#include <vector>

namespace dockx {
namespace sourcedocks {

static bool g_shutdown = false;

static QString dockIdFor(int id)
{
	return QString("dockx_source_%1").arg(id);
}

/* per-dock zoom limits */
static const float ZOOM_MAX = 8.0f;
static const float ZOOM_STEP = 1.15f; /* per wheel notch / menu click */

/* the widget-space view of the source for a given zoom + pan. zoom = 1 is
   exactly the old centered letterbox; zoomed in, (panX, panY) is the source
   fraction sitting at the widget's center, clamped so the view never
   scrolls past the source. Used IDENTICALLY by the draw callback and the
   input mapping so clicks stay accurate while zoomed. */
static void viewRect(float cx, float cy, float w, float h, float zoom, float px, float py, float &scale, float &vx,
		     float &vy)
{
	scale = qMin(cx / w, cy / h) * zoom;
	const float vw = scale * w, vh = scale * h;
	if (vw <= cx)
		px = 0.5f;
	else
		px = qBound(cx / (2.0f * vw), px, 1.0f - cx / (2.0f * vw));
	if (vh <= cy)
		py = 0.5f;
	else
		py = qBound(cy / (2.0f * vh), py, 1.0f - cy / (2.0f * vh));
	vx = cx / 2.0f - px * vw;
	vy = cy / 2.0f - py * vh;
}

static uint32_t toObsModifiers(Qt::KeyboardModifiers m, Qt::MouseButtons buttons)
{
	uint32_t mods = INTERACT_NONE;
	if (m & Qt::ShiftModifier)
		mods |= INTERACT_SHIFT_KEY;
	if (m & Qt::ControlModifier)
		mods |= INTERACT_CONTROL_KEY;
	if (m & Qt::AltModifier)
		mods |= INTERACT_ALT_KEY;
	if (buttons & Qt::LeftButton)
		mods |= INTERACT_MOUSE_LEFT;
	if (buttons & Qt::MiddleButton)
		mods |= INTERACT_MOUSE_MIDDLE;
	if (buttons & Qt::RightButton)
		mods |= INTERACT_MOUSE_RIGHT;
	return mods;
}

/* the native paint surface: obs_display + draw callback + input forwarding */
class VideoWidget : public QWidget {
public:
	int id;
	int kind;
	QString sourceName;
	bool interactive = false; /* forward mouse/keys into the source */

	VideoWidget(int id_, int kind_, const QString &name, QWidget *parent)
		: QWidget(parent),
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
		setMouseTracking(true);
		setFocusPolicy(Qt::ClickFocus);
		for (const SourceDockEntry &e : state().sourceDocks) {
			if (e.id == id_) {
				zoom = (float)e.zoom;
				panX = (float)e.panX;
				panY = (float)e.panY;
				break;
			}
		}
	}

	~VideoWidget() override
	{
		destroyDisplay();
		releaseShowing();
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
		obs_display_remove_draw_callback(display, &VideoWidget::drawCb, this);
		obs_display_destroy(display);
		display = nullptr;
	}

	void clearWeak() { setWeak(nullptr); }

	/* tell OBS the source is on screen even when it is not in the current
	   scene; without this cameras/browsers/media that deactivate when
	   hidden go dark in their dock */
	void updateShowing()
	{
		obs_source_t *cur = lockSource();
		if (cur && isVisible()) {
			if (shownSrc != cur) {
				releaseShowing();
				shownSrc = obs_source_get_ref(cur);
				if (shownSrc)
					obs_source_inc_showing(shownSrc);
			}
		} else {
			releaseShowing();
		}
		if (cur)
			obs_source_release(cur);
	}

	void releaseShowing()
	{
		if (!shownSrc)
			return;
		obs_source_dec_showing(shownSrc);
		obs_source_release(shownSrc);
		shownSrc = nullptr;
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		ensureDisplay();
		if (display)
			obs_display_set_enabled(display, true);
		updateShowing();
	}

	void hideEvent(QHideEvent *e) override
	{
		QWidget::hideEvent(e);
		if (display)
			obs_display_set_enabled(display, false); /* no GPU work while hidden */
		updateShowing();
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (display) {
			const qreal dpr = devicePixelRatioF();
			obs_display_resize(display, (uint32_t)(width() * dpr), (uint32_t)(height() * dpr));
		}
	}

	/* ---- input forwarding (browser sources etc.) + zoom gestures ---- */

	void mousePressEvent(QMouseEvent *e) override
	{
		/* middle-drag always pans; left-drag pans when zoomed in and the
		   source does not take clicks itself */
		if (e->button() == Qt::MiddleButton ||
		    (!interactive && e->button() == Qt::LeftButton && zoom.load() > 1.001f)) {
			panning = true;
			panPress = e->position();
			panAtPress = QPointF(panX.load(), panY.load());
			setCursor(Qt::ClosedHandCursor);
			e->accept();
			return;
		}
		sendClick(e, false);
	}

	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (panning && (e->button() == Qt::MiddleButton || e->button() == Qt::LeftButton)) {
			panning = false;
			unsetCursor();
			scheduleSave();
			e->accept();
			return;
		}
		sendClick(e, true);
	}

	void mouseDoubleClickEvent(QMouseEvent *e) override
	{
		/* double-click resets to fit (Ctrl+double-click when the source
		   takes clicks itself, so browser double-clicks still work) */
		if (!interactive || (e->modifiers() & Qt::ControlModifier)) {
			resetZoom();
			e->accept();
			return;
		}
		sendClick(e, false, 2);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (panning) {
			uint32_t w, h;
			if (sourceSize(w, h)) {
				const qreal dpr = devicePixelRatioF();
				const float cx = (float)(width() * dpr), cy = (float)(height() * dpr);
				const float scale = qMin(cx / (float)w, cy / (float)h) * zoom.load();
				const float vw = scale * (float)w, vh = scale * (float)h;
				const QPointF d = (e->position() - panPress) * dpr;
				if (vw > 0.0f)
					panX = (float)(panAtPress.x() - d.x() / vw);
				if (vh > 0.0f)
					panY = (float)(panAtPress.y() - d.y() / vh);
			}
			e->accept();
			return;
		}
		if (!interactive)
			return;
		if (obs_source_t *src = lockSource()) {
			obs_mouse_event me = {};
			me.modifiers = toObsModifiers(e->modifiers(), e->buttons());
			if (mapToSource(e->position(), me.x, me.y))
				obs_source_send_mouse_move(src, &me, false);
			obs_source_release(src);
		}
	}

	void leaveEvent(QEvent *e) override
	{
		QWidget::leaveEvent(e);
		if (!interactive)
			return;
		if (obs_source_t *src = lockSource()) {
			obs_mouse_event me = {};
			obs_source_send_mouse_move(src, &me, true);
			obs_source_release(src);
		}
	}

	void wheelEvent(QWheelEvent *e) override
	{
		/* wheel zooms; on sources that use the wheel themselves
		   (browsers scroll) hold Ctrl to zoom instead */
		if (!interactive || (e->modifiers() & Qt::ControlModifier)) {
			const int notches = e->angleDelta().y() / 120;
			if (notches != 0)
				zoomAt(e->position(), std::pow(ZOOM_STEP, (float)notches));
			e->accept();
			return;
		}
		if (obs_source_t *src = lockSource()) {
			obs_mouse_event me = {};
			me.modifiers = toObsModifiers(e->modifiers(), e->buttons());
			mapToSource(e->position(), me.x, me.y);
			obs_source_send_mouse_wheel(src, &me, e->angleDelta().x(), e->angleDelta().y());
			obs_source_release(src);
			e->accept();
		}
	}

	void contextMenuEvent(QContextMenuEvent *e) override
	{
		/* interactive sources get right-clicks forwarded instead */
		if (interactive) {
			QWidget::contextMenuEvent(e);
			return;
		}
		QMenu menu(this);
		QAction *in = menu.addAction("Zoom in");
		QAction *out = menu.addAction("Zoom out");
		QAction *reset = menu.addAction("Reset zoom (fit)");
		out->setEnabled(zoom.load() > 1.001f);
		reset->setEnabled(zoom.load() > 1.001f);
		QAction *picked = menu.exec(e->globalPos());
		const QPointF center(width() / 2.0, height() / 2.0);
		if (picked == in)
			zoomAt(center, ZOOM_STEP);
		else if (picked == out)
			zoomAt(center, 1.0f / ZOOM_STEP);
		else if (picked == reset)
			resetZoom();
		e->accept();
	}

	void keyPressEvent(QKeyEvent *e) override { sendKey(e, false); }
	void keyReleaseEvent(QKeyEvent *e) override { sendKey(e, true); }

	void focusInEvent(QFocusEvent *e) override
	{
		QWidget::focusInEvent(e);
		sendFocus(true);
	}

	void focusOutEvent(QFocusEvent *e) override
	{
		QWidget::focusOutEvent(e);
		sendFocus(false);
	}

private:
	obs_display_t *display = nullptr;
	obs_weak_source_t *weak = nullptr;
	obs_source_t *shownSrc = nullptr; /* holds the inc_showing ref */
	std::mutex weakMutex;

	/* zoom + pan: written on the UI thread, read by the draw callback on
	   the graphics thread, hence atomics */
	std::atomic<float> zoom{1.0f};
	std::atomic<float> panX{0.5f};
	std::atomic<float> panY{0.5f};
	bool panning = false;
	QPointF panPress;
	QPointF panAtPress;
	QTimer *saveTimer = nullptr;

	/* the rendered pixel size, UI thread */
	bool sourceSize(uint32_t &w, uint32_t &h)
	{
		if (kind == KIND_PROGRAM) {
			obs_video_info ovi;
			if (!obs_get_video_info(&ovi))
				return false;
			w = ovi.base_width;
			h = ovi.base_height;
			return w && h;
		}
		obs_source_t *src = lockSource();
		if (!src)
			return false;
		w = obs_source_get_width(src);
		h = obs_source_get_height(src);
		obs_source_release(src);
		return w && h;
	}

	/* zoom by a factor keeping the source point under `pos` in place */
	void zoomAt(const QPointF &pos, float factor)
	{
		uint32_t w, h;
		if (!sourceSize(w, h))
			return;
		const qreal dpr = devicePixelRatioF();
		const float cx = (float)(width() * dpr), cy = (float)(height() * dpr);
		float scale, vx, vy;
		viewRect(cx, cy, (float)w, (float)h, zoom.load(), panX.load(), panY.load(), scale, vx, vy);
		const float mx = (float)(pos.x() * dpr), my = (float)(pos.y() * dpr);
		const float sx = (mx - vx) / scale, sy = (my - vy) / scale; /* source pt under cursor */
		const float nz = qBound(1.0f, zoom.load() * factor, ZOOM_MAX);
		zoom = nz;
		const float fit = qMin(cx / (float)w, cy / (float)h);
		const float vw2 = fit * nz * (float)w, vh2 = fit * nz * (float)h;
		if (vw2 > 0.0f && vh2 > 0.0f) {
			/* keep (sx, sy) under the cursor: solve for the new pan */
			panX = (cx / 2.0f - (mx - sx * fit * nz)) / vw2;
			panY = (cy / 2.0f - (my - sy * fit * nz)) / vh2;
		}
		scheduleSave();
	}

	void resetZoom()
	{
		zoom = 1.0f;
		panX = 0.5f;
		panY = 0.5f;
		scheduleSave();
	}

	/* wheel spam writes once, 600ms after the last change */
	void scheduleSave()
	{
		if (!saveTimer) {
			saveTimer = new QTimer(this);
			saveTimer->setSingleShot(true);
			saveTimer->setInterval(600);
			QObject::connect(saveTimer, &QTimer::timeout, this, [this]() {
				for (SourceDockEntry &e : state().sourceDocks) {
					if (e.id == id) {
						e.zoom = (double)zoom.load();
						e.panX = (double)panX.load();
						e.panY = (double)panY.load();
						break;
					}
				}
				stateSave();
			});
		}
		saveTimer->start();
	}

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
				"source dock %d: display unsupported on this platform (Wayland needs Qt 6.9+)", id);
			return;
		}
		display = obs_display_create(&info, 0x151515);
		if (display)
			obs_display_add_draw_callback(display, &VideoWidget::drawCb, this);
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
		QWidget *p = parentWidget();
		while (p && !qobject_cast<QDockWidget *>(p))
			p = p->parentWidget();
		if (p)
			p->setWindowTitle(n);
	}

	/* widget position -> source pixel position (undo the letterbox math,
	   zoom + pan included so clicks stay accurate while zoomed) */
	bool mapToSource(const QPointF &pos, int32_t &sx, int32_t &sy)
	{
		uint32_t w, h;
		if (!sourceSize(w, h))
			return false;
		const qreal dpr = devicePixelRatioF();
		const float cx = (float)(width() * dpr);
		const float cy = (float)(height() * dpr);
		float scale, vx, vy;
		viewRect(cx, cy, (float)w, (float)h, zoom.load(), panX.load(), panY.load(), scale, vx, vy);
		if (scale <= 0.0f)
			return false;
		sx = (int32_t)(((float)(pos.x() * dpr) - vx) / scale);
		sy = (int32_t)(((float)(pos.y() * dpr) - vy) / scale);
		sx = qBound(0, sx, (int32_t)w - 1);
		sy = qBound(0, sy, (int32_t)h - 1);
		return true;
	}

	void sendClick(QMouseEvent *e, bool up, uint32_t clicks = 1)
	{
		if (!interactive) {
			if (up)
				QWidget::mouseReleaseEvent(e);
			else
				QWidget::mousePressEvent(e);
			return;
		}
		obs_source_t *src = lockSource();
		if (!src)
			return;
		obs_mouse_event me = {};
		me.modifiers = toObsModifiers(e->modifiers(), e->buttons());
		if (mapToSource(e->position(), me.x, me.y)) {
			int32_t btn = MOUSE_LEFT;
			if (e->button() == Qt::RightButton)
				btn = MOUSE_RIGHT;
			else if (e->button() == Qt::MiddleButton)
				btn = MOUSE_MIDDLE;
			obs_source_send_mouse_click(src, &me, btn, up, clicks);
		}
		obs_source_release(src);
		if (!up)
			setFocus();
	}

	void sendKey(QKeyEvent *e, bool up)
	{
		if (!interactive) {
			if (up)
				QWidget::keyReleaseEvent(e);
			else
				QWidget::keyPressEvent(e);
			return;
		}
		obs_source_t *src = lockSource();
		if (!src)
			return;
		QByteArray text = e->text().toUtf8();
		obs_key_event ke = {};
		ke.modifiers = toObsModifiers(e->modifiers(), Qt::NoButton);
		ke.text = text.data();
		ke.native_modifiers = e->nativeModifiers();
		ke.native_scancode = e->nativeScanCode();
		ke.native_vkey = e->nativeVirtualKey();
		obs_source_send_key_click(src, &ke, up);
		obs_source_release(src);
	}

	void sendFocus(bool focused)
	{
		if (!interactive)
			return;
		if (obs_source_t *src = lockSource()) {
			obs_source_send_focus(src, focused);
			obs_source_release(src);
		}
	}

	/* graphics thread: libobs calls only */
	static void drawCb(void *param, uint32_t cx, uint32_t cy)
	{
		/* never render a user scene while OBS is loading or switching
		   collections (see obsReady() in dockx.hpp: startup crash fix) */
		if (!obsReady())
			return;
		VideoWidget *v = static_cast<VideoWidget *>(param);
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
		/* same view math as the input mapping: full-widget viewport, the
		   ortho window crops to the zoomed + panned source region (at
		   zoom 1 this is exactly the old centered letterbox) */
		float scale, vx, vy;
		viewRect((float)cx, (float)cy, (float)w, (float)h, v->zoom.load(), v->panX.load(), v->panY.load(),
			 scale, vx, vy);
		if (scale <= 0.0f) {
			if (src)
				obs_source_release(src);
			return;
		}
		gs_viewport_push();
		gs_projection_push();
		gs_ortho((0.0f - vx) / scale, ((float)cx - vx) / scale, (0.0f - vy) / scale, ((float)cy - vy) / scale,
			 -100.0f, 100.0f);
		gs_set_viewport(0, 0, (int)cx, (int)cy);
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

/* the dock content: video on top, audio controls underneath when the source
   has audio (audio only sources show just the control strip) */
class SourceDockPanel : public QWidget {
public:
	VideoWidget *video;

	SourceDockPanel(int id, int kind, const QString &name) : QWidget(nullptr)
	{
		QVBoxLayout *v = new QVBoxLayout(this);
		v->setContentsMargins(0, 0, 0, 0);
		v->setSpacing(2);
		video = new VideoWidget(id, kind, name, this);
		v->addWidget(video, 1);

		audioRow = new QWidget(this);
		QHBoxLayout *h = new QHBoxLayout(audioRow);
		h->setContentsMargins(6, 2, 6, 4);
		h->setSpacing(6);
		muteBtn = new QToolButton(audioRow);
		muteBtn->setCheckable(true);
		muteBtn->setAutoRaise(true);
		muteBtn->setToolTip("Mute");
		h->addWidget(muteBtn);
		volSlider = new QSlider(Qt::Horizontal, audioRow);
		volSlider->setRange(0, 100);
		volSlider->setToolTip("Volume");
		h->addWidget(volSlider, 1);
		v->addWidget(audioRow);
		audioRow->setVisible(false);

		QObject::connect(volSlider, &QSlider::valueChanged, this, [this](int val) {
			if (syncing)
				return;
			if (obs_source_t *src = video->lockSource()) {
				const float f = (float)val / 100.0f;
				obs_source_set_volume(src, f * f * f); /* cubic, like OBS */
				obs_source_release(src);
			}
		});
		QObject::connect(muteBtn, &QToolButton::toggled, this, [this](bool on) {
			if (syncing)
				return;
			if (obs_source_t *src = video->lockSource()) {
				obs_source_set_muted(src, on);
				obs_source_release(src);
			}
			updateMuteIcon(on);
		});

		/* the mixer can change volume/mute behind our back; keep in sync
		   with a light poll while the controls are on screen */
		poll = new QTimer(this);
		poll->setInterval(800);
		QObject::connect(poll, &QTimer::timeout, this, [this]() { syncControls(); });
	}

	void refresh()
	{
		video->resolve();
		video->updateShowing();
		uint32_t flags = 0;
		if (obs_source_t *src = video->lockSource()) {
			flags = obs_source_get_output_flags(src);
			obs_source_release(src);
		}
		const bool audio = video->kind == KIND_SOURCE && (flags & OBS_SOURCE_AUDIO);
		const bool videoOut = video->kind != KIND_SOURCE || (flags & OBS_SOURCE_VIDEO);
		video->interactive = (flags & OBS_SOURCE_INTERACTION) != 0;
		video->setVisible(videoOut);
		audioRow->setVisible(audio);
		if (audio) {
			syncControls();
			poll->start();
		} else {
			poll->stop();
		}
	}

	void teardown()
	{
		poll->stop();
		video->destroyDisplay();
		video->releaseShowing();
		video->clearWeak();
	}

private:
	QWidget *audioRow;
	QSlider *volSlider;
	QToolButton *muteBtn;
	QTimer *poll;
	bool syncing = false;

	void updateMuteIcon(bool muted)
	{
		muteBtn->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
	}

	void syncControls()
	{
		obs_source_t *src = video->lockSource();
		if (!src)
			return;
		const float vol = obs_source_get_volume(src);
		const bool muted = obs_source_muted(src);
		obs_source_release(src);
		syncing = true;
		volSlider->setValue((int)std::lround(std::cbrt((double)vol) * 100.0));
		muteBtn->setChecked(muted);
		updateMuteIcon(muted);
		syncing = false;
	}
};

static std::vector<SourceDockPanel *> g_views;

static QString titleFor(const SourceDockEntry &e)
{
	if (e.kind == KIND_PROGRAM)
		return QStringLiteral("Program");
	if (e.kind == KIND_PREVIEW)
		return QStringLiteral("Preview");
	return e.sourceName;
}

static SourceDockPanel *registerView(const SourceDockEntry &e)
{
	SourceDockPanel *v = new SourceDockPanel(e.id, e.kind, e.sourceName);
	if (!obs_frontend_add_dock_by_id(dockIdFor(e.id).toUtf8().constData(), titleFor(e).toUtf8().constData(), v)) {
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
	for (SourceDockPanel *v : g_views)
		v->refresh();
}

void addDock(int kind, const QString &sourceName)
{
	SourceDockEntry e;
	e.id = state().nextSourceDockId++;
	e.kind = kind;
	e.sourceName = sourceName;
	state().sourceDocks.push_back(e);
	stateSave();
	SourceDockPanel *v = registerView(e);
	if (!v)
		return;
	v->refresh();
	/* pop the new dock open so it visibly appears */
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(dockIdFor(e.id)) : nullptr;
	if (dock) {
		dock->setVisible(true);
		dock->raise();
	}
}

/* bring every Program/Preview dock back on screen (a closed dock stays closed
   forever in OBS); returns false when none exist so the caller can offer one */
bool showVideoDocks()
{
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!m)
		return false;
	bool any = false;
	for (const SourceDockEntry &e : state().sourceDocks) {
		if (e.kind != KIND_PROGRAM && e.kind != KIND_PREVIEW)
			continue;
		QDockWidget *dock = m->findChild<QDockWidget *>(dockIdFor(e.id));
		if (dock) {
			dock->setVisible(true);
			dock->raise();
			any = true;
		}
	}
	return any;
}

void removeDock(int id)
{
	for (auto it = g_views.begin(); it != g_views.end(); ++it) {
		if ((*it)->video->id == id) {
			(*it)->teardown();
			g_views.erase(it);
			break;
		}
	}
	/* removing the dock destroys the wrapper AND our panel widget */
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
	for (SourceDockPanel *v : g_views)
		v->teardown();
	g_views.clear();
}

} // namespace sourcedocks
} // namespace dockx
