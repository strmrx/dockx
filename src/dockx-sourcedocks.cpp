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

#include <QDockWidget>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMainWindow>
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
	}

	~VideoWidget() override
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
		obs_display_remove_draw_callback(display, &VideoWidget::drawCb, this);
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

	/* ---- input forwarding (browser sources etc.) ---- */

	void mousePressEvent(QMouseEvent *e) override { sendClick(e, false); }
	void mouseReleaseEvent(QMouseEvent *e) override { sendClick(e, true); }
	void mouseDoubleClickEvent(QMouseEvent *e) override { sendClick(e, false, 2); }

	void mouseMoveEvent(QMouseEvent *e) override
	{
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
		if (!interactive) {
			QWidget::wheelEvent(e);
			return;
		}
		if (obs_source_t *src = lockSource()) {
			obs_mouse_event me = {};
			me.modifiers = toObsModifiers(e->modifiers(), e->buttons());
			mapToSource(e->position(), me.x, me.y);
			obs_source_send_mouse_wheel(src, &me, e->angleDelta().x(),
						    e->angleDelta().y());
			obs_source_release(src);
			e->accept();
		}
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

	/* widget position -> source pixel position (undo the letterbox math) */
	bool mapToSource(const QPointF &pos, int32_t &sx, int32_t &sy)
	{
		obs_source_t *src = lockSource();
		if (!src)
			return false;
		const uint32_t w = obs_source_get_width(src);
		const uint32_t h = obs_source_get_height(src);
		obs_source_release(src);
		if (!w || !h)
			return false;
		const qreal dpr = devicePixelRatioF();
		const float cx = (float)(width() * dpr);
		const float cy = (float)(height() * dpr);
		const float scale = qMin(cx / (float)w, cy / (float)h);
		if (scale <= 0.0f)
			return false;
		const float vx = (cx - scale * (float)w) / 2.0f;
		const float vy = (cy - scale * (float)h) / 2.0f;
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
		const float scale = qMin((float)cx / (float)w, (float)cy / (float)h);
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
		muteBtn->setIcon(style()->standardIcon(
			muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
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
