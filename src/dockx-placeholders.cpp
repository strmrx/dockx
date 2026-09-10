/*
DockX for OBS Studio (by StrmrX) -- placeholder docks.
GPL v2, see plugin-main.cpp for the full notice.

An empty labeled dock that reserves a spot in the layout for a window OBS
cannot host (TikTok Live Studio chat, a music player, any app floated over
OBS). A spot can also SHOW A LOCAL FILE instead -- an image, a GIF, or a
looping muted video (branding, logos, set dressing), on every platform.
On Windows a real window can be PINNED: it becomes an OWNED window of
the OBS main window, so the shell stacks it as part of OBS (just above OBS,
under whatever app the user selects, hidden when OBS minimizes), while a
timer moves + sizes it to sit exactly over the placeholder through dock
drags, layout switches and OBS restarts. The foreign window is only ever
repositioned/owner-tagged via SetWindowPos/SetWindowLongPtr, never
reparented into OBS's widget tree -- reparenting a window another process
owns is the classic way to crash both apps, and a broken plugin takes the
whole stream down with it.
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include "dockx.hpp"

#include <QColor>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMovie>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <cmath>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace dockx {
namespace placeholders {

static QString dockIdFor(int id)
{
	return QString("dockx_placeholder_%1").arg(id);
}

static PlaceholderEntry *entryFor(int id)
{
	for (PlaceholderEntry &e : state().placeholders)
		if (e.id == id)
			return &e;
	return nullptr;
}

static QString titleFor(const PlaceholderEntry &e)
{
	return e.label.isEmpty() ? QString("Placeholder") : e.label;
}

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* ---------- the pinned-window follower (Windows only) ---------- */

#ifdef _WIN32

static bool eligibleWindow(HWND h)
{
	if (!IsWindowVisible(h))
		return false;
	if (GetWindow(h, GW_OWNER) != nullptr)
		return false;
	const LONG_PTR ex = GetWindowLongPtr(h, GWL_EXSTYLE);
	if (ex & WS_EX_TOOLWINDOW)
		return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == GetCurrentProcessId())
		return false;
	return GetWindowTextLengthW(h) > 0;
}

static QString windowTitle(HWND h)
{
	wchar_t buf[512];
	const int n = GetWindowTextW(h, buf, 512);
	return QString::fromWCharArray(buf, n);
}

static QString exeBaseName(HWND h)
{
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (!pid)
		return QString();
	HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return QString();
	wchar_t buf[MAX_PATH];
	DWORD len = MAX_PATH;
	QString out;
	if (QueryFullProcessImageNameW(proc, 0, buf, &len))
		out = QString::fromWCharArray(buf, (int)len);
	CloseHandle(proc);
	const int slash = out.lastIndexOf(QChar('\\'));
	return (slash >= 0 ? out.mid(slash + 1) : out).toLower();
}

struct PinFindCtx {
	QString wantedTitle;
	QString wantedExe;             /* lower-case basename; empty = don't check */
	QList<QPair<HWND, bool>> hits; /* hwnd, exact-title match */
};

static BOOL CALLBACK pinFindCb(HWND h, LPARAM lp)
{
	PinFindCtx *ctx = reinterpret_cast<PinFindCtx *>(lp);
	if (!eligibleWindow(h))
		return TRUE;
	const QString t = windowTitle(h);
	const bool exact = (t == ctx->wantedTitle);
	if (!exact && !t.contains(ctx->wantedTitle, Qt::CaseInsensitive))
		return TRUE;
	if (!ctx->wantedExe.isEmpty() && exeBaseName(h) != ctx->wantedExe)
		return TRUE;
	ctx->hits.append({h, exact});
	return TRUE;
}

/* Re-find the pinned window after it (or OBS) went away. Apps like TikTok
   Live Studio re-dock their popped-out chat on restart, so for a while the
   only title match is the app's whole MAIN window -- and yanking that into
   the slot is far worse than waiting for the user to pop the chat back out
   (Joey hit exactly this). Titles are often IDENTICAL between a panel and
   its main window, so the pin remembers the window's size from when it was
   picked: a candidate much bigger than that in EITHER dimension reads as
   the main window -- skip it and wait. (The old spot-based cap failed Joey
   because it required too-big in BOTH dimensions; a tall skinny chat slot
   made the height cap huge and the main window slipped through.) Pins from
   before the size was stored keep the legacy spot cap. Among survivors
   prefer exact title, then the size closest to the remembered one (else
   smallest -- panels are small, main windows are big). */
static HWND findPinTarget(const QString &title, const QString &exe, int savedW, int savedH, int spotW, int spotH)
{
	PinFindCtx ctx;
	ctx.wantedTitle = title;
	ctx.wantedExe = exe;
	EnumWindows(pinFindCb, reinterpret_cast<LPARAM>(&ctx));
	const bool haveSaved = savedW > 0 && savedH > 0;
	const int capW = haveSaved ? qMax(2 * savedW, 480) : qMax(2 * spotW, 800);
	const int capH = haveSaved ? qMax(2 * savedH, 480) : qMax(2 * spotH, 800);
	const long long savedArea = (long long)savedW * savedH;
	HWND best = nullptr;
	bool bestExact = false;
	long long bestScore = 0;
	for (const auto &hit : ctx.hits) {
		RECT r = {};
		if (!GetWindowRect(hit.first, &r))
			continue;
		const int w = (int)(r.right - r.left), hgt = (int)(r.bottom - r.top);
		if (haveSaved) {
			if (w > capW || hgt > capH)
				continue; /* far bigger than what was pinned = the main window */
		} else if (w > capW && hgt > capH) {
			continue; /* legacy pin without a stored size */
		}
		const long long area = (long long)w * hgt;
		const long long score = haveSaved ? qAbs(area - savedArea) : area;
		if (!best || (hit.second && !bestExact) || (hit.second == bestExact && score < bestScore)) {
			best = hit.first;
			bestExact = hit.second;
			bestScore = score;
		}
	}
	return best;
}

struct ListCtx {
	QList<QPair<QString, quintptr>> rows;
};

static BOOL CALLBACK listWindowsCb(HWND h, LPARAM lp)
{
	ListCtx *ctx = reinterpret_cast<ListCtx *>(lp);
	if (eligibleWindow(h))
		ctx->rows.append({windowTitle(h), reinterpret_cast<quintptr>(h)});
	return TRUE;
}

static QList<QPair<QString, quintptr>> listWindows()
{
	ListCtx ctx;
	EnumWindows(listWindowsCb, reinterpret_cast<LPARAM>(&ctx));
	return ctx.rows;
}

#endif /* _WIN32 */

/* ---------- media in a spot (image / GIF / looping video) ---------- */

enum class MediaKind { Image, Anim, Video };

static MediaKind mediaKindFor(const QString &path)
{
	const QString ext = QFileInfo(path).suffix().toLower();
	if (ext == QLatin1String("gif") || ext == QLatin1String("apng"))
		return MediaKind::Anim;
	static const QStringList still = {"png", "jpg", "jpeg", "bmp", "webp", "svg", "tif", "tiff", "ico"};
	if (still.contains(ext))
		return MediaKind::Image;
	return MediaKind::Video;
}

/* video plays through a private OBS media source rendered straight into the
   dock (same display pattern as the source docks) -- OBS is already a great
   video player, so reuse it. Private = it never appears in the user's scenes
   or mixer, and it is muted: this is set dressing, not program audio */
class MediaVideoWidget : public QWidget {
public:
	MediaVideoWidget(const QString &path, const QString &mode, QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_StaticContents);
		setAttribute(Qt::WA_NoSystemBackground);
		setAttribute(Qt::WA_OpaquePaintEvent);
		setAttribute(Qt::WA_DontCreateNativeAncestors);
		setAttribute(Qt::WA_NativeWindow);
		/* clicks fall through so the placeholder keeps its right click menu */
		setAttribute(Qt::WA_TransparentForMouseEvents);
		setMode(mode);
		obs_data_t *s = obs_data_create();
		obs_data_set_string(s, "local_file", path.toUtf8().constData());
		obs_data_set_bool(s, "looping", true);
		obs_data_set_bool(s, "restart_on_activate", false);
		obs_data_set_bool(s, "close_when_inactive", false);
		obs_data_set_bool(s, "hw_decode", true);
		source = obs_source_create_private("ffmpeg_source", "dockx_placeholder_media", s);
		obs_data_release(s);
		if (source)
			obs_source_set_muted(source, true);
		else
			obs_log(LOG_WARNING, "placeholder media: could not create a player for \"%s\"",
				path.toUtf8().constData());
	}

	~MediaVideoWidget() override { releasePlayer(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	void setMode(const QString &m) { fillMode.store(m == QLatin1String("fill")); }

	/* tear down while OBS is still up; safe to call twice */
	void releasePlayer()
	{
		if (display) {
			obs_display_remove_draw_callback(display, &MediaVideoWidget::drawCb, this);
			obs_display_destroy(display);
			display = nullptr;
		}
		if (source) {
			if (shown)
				obs_source_dec_showing(source);
			shown = false;
			obs_source_release(source);
			source = nullptr;
		}
	}

protected:
	void showEvent(QShowEvent *ev) override
	{
		QWidget::showEvent(ev);
		ensureDisplay();
		if (display)
			obs_display_set_enabled(display, true);
		if (source && !shown) {
			obs_source_inc_showing(source);
			shown = true;
		}
	}

	void hideEvent(QHideEvent *ev) override
	{
		QWidget::hideEvent(ev);
		if (display)
			obs_display_set_enabled(display, false); /* no GPU work while hidden */
		if (source && shown) {
			obs_source_dec_showing(source);
			shown = false;
		}
	}

	void resizeEvent(QResizeEvent *ev) override
	{
		QWidget::resizeEvent(ev);
		if (display) {
			const qreal dpr = devicePixelRatioF();
			obs_display_resize(display, (uint32_t)(width() * dpr), (uint32_t)(height() * dpr));
		}
	}

private:
	obs_display_t *display = nullptr;
	obs_source_t *source = nullptr;
	bool shown = false;
	std::atomic<bool> fillMode{false};

	void ensureDisplay()
	{
		if (display || !source)
			return;
		const qreal dpr = devicePixelRatioF();
		gs_init_data info = {};
		info.cx = (uint32_t)(width() > 0 ? width() * dpr : 8);
		info.cy = (uint32_t)(height() > 0 ? height() * dpr : 8);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
		if (!wireDisplayWindow(info, (quintptr)winId())) {
			obs_log(LOG_WARNING, "placeholder media: display unsupported on this platform");
			return;
		}
		display = obs_display_create(&info, 0x151515);
		if (display)
			obs_display_add_draw_callback(display, &MediaVideoWidget::drawCb, this);
		else
			obs_log(LOG_WARNING, "placeholder media: display create failed");
	}

	/* graphics thread: libobs calls only. The source pointer is fixed for
	   the widget's life and the callback is removed before it is released */
	static void drawCb(void *param, uint32_t cx, uint32_t cy)
	{
		/* never render while OBS is loading or switching collections
		   (see obsReady() in dockx.hpp: startup crash fix) */
		if (!obsReady())
			return;
		MediaVideoWidget *v = static_cast<MediaVideoWidget *>(param);
		obs_source_t *src = v->source;
		if (!src)
			return;
		const uint32_t w = obs_source_get_width(src), h = obs_source_get_height(src);
		if (!w || !h)
			return;
		const float sx = (float)cx / (float)w, sy = (float)cy / (float)h;
		const float scale = v->fillMode.load() ? qMax(sx, sy) : qMin(sx, sy);
		const int vw = (int)(scale * (float)w), vh = (int)(scale * (float)h);
		const int vx = ((int)cx - vw) / 2, vy = ((int)cy - vh) / 2;
		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		obs_source_video_render(src);
		gs_projection_pop();
		gs_viewport_pop();
	}
};

/* ---------- the dock widget ---------- */

class PlaceholderPanel : public QWidget {
public:
	int id;

	explicit PlaceholderPanel(int entryId) : id(entryId)
	{
		setObjectName(QString("dockx_placeholder_panel_%1").arg(id));
		setMinimumSize(80, 60);
#ifdef _WIN32
		/* a native handle so GetWindowRect gives the exact on-screen
		   rect, DPI and multi-monitor scaling included */
		setAttribute(Qt::WA_NativeWindow, true);
		setAttribute(Qt::WA_DontCreateNativeAncestors, true);
		timer = new QTimer(this);
		timer->setInterval(250);
		QObject::connect(timer, &QTimer::timeout, this, [this]() { tick(); });
		timer->start();
#endif
		refreshMedia();
	}

	~PlaceholderPanel() override
	{
		releaseTarget();
		releaseMedia();
	}

	/* (re)build the media members from the entry; cheap when unchanged */
	void refreshMedia()
	{
		const PlaceholderEntry *e = entryFor(id);
		const QString path = e ? e->mediaPath : QString();
		const QString mode = e ? e->mediaMode : QString();
		if (path == mediaLoaded) {
			if (mediaVideo)
				mediaVideo->setMode(mode);
			update();
			return;
		}
		releaseMedia();
		mediaLoaded = path;
		if (path.isEmpty()) {
			update();
			return;
		}
		if (!QFileInfo::exists(path)) {
			mediaFailed = true;
			update();
			return;
		}
		switch (mediaKindFor(path)) {
		case MediaKind::Anim:
			mediaMovie = new QMovie(path, QByteArray(), this);
			if (!mediaMovie->isValid()) {
				delete mediaMovie;
				mediaMovie = nullptr;
				mediaFailed = true;
				break;
			}
			QObject::connect(mediaMovie, &QMovie::frameChanged, this, [this](int) { update(); });
			mediaMovie->start();
			if (!isVisible())
				mediaMovie->setPaused(true);
			break;
		case MediaKind::Image:
			if (!mediaPixmap.load(path))
				mediaFailed = true;
			break;
		case MediaKind::Video:
			mediaVideo = new MediaVideoWidget(path, mode, this);
			mediaVideo->setGeometry(rect());
			mediaVideo->show();
			break;
		}
		update();
	}

	void releaseMedia()
	{
		if (mediaMovie) {
			mediaMovie->stop();
			delete mediaMovie;
			mediaMovie = nullptr;
		}
		if (mediaVideo) {
			mediaVideo->releasePlayer();
			delete mediaVideo;
			mediaVideo = nullptr;
		}
		mediaPixmap = QPixmap();
		mediaLoaded.clear();
		mediaFailed = false;
	}

	void releaseTarget()
	{
#ifdef _WIN32
		applySeamless(false);
		releaseOwnership();
		if (hwnd && IsWindow(hwnd) && topmostApplied)
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		topmostApplied = false;
		hwnd = nullptr;
		setMinimumSize(80, 60); /* forget a learned window minimum */
#endif
	}

	void adoptTarget(quintptr h)
	{
#ifdef _WIN32
		releaseTarget();
		hwnd = reinterpret_cast<HWND>(h);
#else
		(void)h;
#endif
	}

	/* user-triggered escape hatch for a stale learned minimum (the app
	   got smaller inside, the dock stayed stuck large); safe to fire any
	   time because a real minimum relearns on the next tick */
	void resetSizeLimit()
	{
		setMinimumSize(80, 60);
#ifdef _WIN32
		minForgetTicks = 0;
#endif
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		const PlaceholderEntry *e = entryFor(id);
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);

		QColor bg = palette().color(QPalette::Window).darker(115);
		if (e && !e->color.isEmpty()) {
			const QColor c(e->color);
			if (c.isValid())
				bg = c;
		}
		p.fillRect(rect(), bg);

		/* media replaces the label + dashed border: the spot IS the content */
		const bool hasMedia = e && !e->mediaPath.isEmpty();
		if (hasMedia && !mediaFailed) {
			if (mediaVideo)
				return; /* the video child covers the spot */
			const QPixmap frame = mediaMovie ? mediaMovie->currentPixmap() : mediaPixmap;
			if (!frame.isNull()) {
				drawMedia(p, frame, e->mediaMode);
				return;
			}
		}

		const bool lightBg = bg.lightness() > 128;
		QColor line = lightBg ? QColor(0, 0, 0, 70) : QColor(255, 255, 255, 60);
		QPen pen(line, 1, Qt::DashLine);
		p.setPen(pen);
		p.drawRoundedRect(rect().adjusted(6, 6, -7, -7), 8, 8);

		QColor text = lightBg ? QColor(0, 0, 0, 200) : QColor(255, 255, 255, 210);
		QFont f = font();
		f.setPointSizeF(f.pointSizeF() + 2);
		f.setBold(true);
		p.setFont(f);
		p.setPen(text);
		const QString label = e ? titleFor(*e) : QString("Placeholder");
		QRect textRect = rect().adjusted(10, 0, -10, 0);
		p.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, label);

		QColor dim = text;
		dim.setAlpha(lightBg ? 110 : 100);
		p.setPen(dim);
		p.setFont(font());
		QString hint;
		const bool pinned = e && !e->pinTitle.isEmpty();
		if (hasMedia && mediaFailed) {
			hint = QString("Could not load: %1").arg(QFileInfo(e->mediaPath).fileName());
		} else if (pinned) {
			hint = QString("Pinned: %1").arg(e->pinTitle);
#ifdef _WIN32
			/* tell the user WHY the spot is empty: the window does
			   not exist yet (e.g. the app pulled its panel back in
			   after a restart) and DockX is deliberately waiting
			   instead of grabbing the app's main window */
			if (!hwnd)
				hint = QString("Waiting for: %1 (open it, or pop the panel back out)").arg(e->pinTitle);
#endif
		} else if (pinningSupported())
			hint = "Right click to pin a window here";
		else
			hint = "Right click for options";
		p.drawText(rect().adjusted(10, 0, -10, -12), Qt::AlignHCenter | Qt::AlignBottom, hint);
	}

	void contextMenuEvent(QContextMenuEvent *ev) override
	{
		PlaceholderEntry *e = entryFor(id);
		if (!e)
			return;
		QMenu menu(this);

		QObject::connect(menu.addAction("Set label..."), &QAction::triggered, this, [this]() {
			PlaceholderEntry *e2 = entryFor(id);
			if (!e2)
				return;
			bool ok = false;
			const QString text = QInputDialog::getText(this, "Placeholder label",
								   "Label:", QLineEdit::Normal, e2->label, &ok);
			if (ok)
				setLabel(id, text.trimmed());
		});
		QObject::connect(menu.addAction("Set background color..."), &QAction::triggered, this, [this]() {
			PlaceholderEntry *e2 = entryFor(id);
			if (!e2)
				return;
			const QColor start = e2->color.isEmpty() ? QColor("#232330") : QColor(e2->color);
			const QColor c = QColorDialog::getColor(start, this, "Placeholder background");
			if (c.isValid())
				setColor(id, c.name());
		});
		if (!e->color.isEmpty()) {
			QObject::connect(menu.addAction("Clear background color"), &QAction::triggered, this,
					 [this]() { setColor(id, QString()); });
		}

		menu.addSeparator();
		if (pinningSupported()) {
			QObject::connect(menu.addAction("Pin a window here..."), &QAction::triggered, this,
					 [this]() { pinWindow(id, this); });
			if (!e->pinTitle.isEmpty()) {
				QAction *un = menu.addAction(QString("Unpin \"%1\"").arg(e->pinTitle));
				QObject::connect(un, &QAction::triggered, this, [this]() { unpinWindow(id); });
				QAction *seam = menu.addAction("Seamless look (hide its title bar)");
				seam->setCheckable(true);
				seam->setChecked(e->seamless);
				QObject::connect(seam, &QAction::triggered, this,
						 [this](bool on) { setSeamless(id, on); });
				QAction *rs = menu.addAction("Reset size limit (relearn how small it fits)");
				QObject::connect(rs, &QAction::triggered, this, [this]() { resetSizeLimit(); });
			}
		} else {
			QAction *na = menu.addAction("Window pinning: Windows only for now");
			na->setEnabled(false);
		}

		menu.addSeparator();
		QObject::connect(menu.addAction("Show an image or video here..."), &QAction::triggered, this,
				 [this]() { chooseMedia(id, this); });
		if (!e->mediaPath.isEmpty()) {
			const bool isVideo = mediaKindFor(e->mediaPath) == MediaKind::Video;
			const QString cur = e->mediaMode.isEmpty() ? QString("fit") : e->mediaMode;
			QMenu *fitMenu = menu.addMenu("How it fills the spot");
			auto addMode = [this, fitMenu, cur](const QString &label, const QString &mode, bool enabled) {
				QAction *a = fitMenu->addAction(label);
				a->setCheckable(true);
				a->setChecked(cur == mode);
				a->setEnabled(enabled);
				QObject::connect(a, &QAction::triggered, this,
						 [this, mode]() { setMediaMode(id, mode); });
			};
			addMode("Fit (show all of it)", "fit", true);
			addMode("Fill (cover the spot, crop the edges)", "fill", true);
			addMode("Tile (repeat it)", "tile", !isVideo);
			QObject::connect(
				menu.addAction(QString("Clear \"%1\"").arg(QFileInfo(e->mediaPath).fileName())),
				&QAction::triggered, this, [this]() { clearMedia(id); });
		}

		menu.addSeparator();
		QObject::connect(menu.addAction("Remove this placeholder"), &QAction::triggered, this,
				 [this]() { QTimer::singleShot(0, [pid = id]() { removeDock(pid); }); });

		menu.exec(ev->globalPos());
	}

	void resizeEvent(QResizeEvent *ev) override
	{
		QWidget::resizeEvent(ev);
		if (mediaVideo)
			mediaVideo->setGeometry(rect());
	}

	void showEvent(QShowEvent *ev) override
	{
		QWidget::showEvent(ev);
		if (mediaMovie)
			mediaMovie->setPaused(false);
	}

	void hideEvent(QHideEvent *ev) override
	{
		QWidget::hideEvent(ev);
		if (mediaMovie)
			mediaMovie->setPaused(true); /* no decode work while hidden */
	}

private:
	QPixmap mediaPixmap;
	QMovie *mediaMovie = nullptr;
	MediaVideoWidget *mediaVideo = nullptr;
	QString mediaLoaded; /* the path the members above were built from */
	bool mediaFailed = false;

	void drawMedia(QPainter &p, const QPixmap &pix, const QString &mode)
	{
		if (pix.width() < 1 || pix.height() < 1)
			return;
		p.setRenderHint(QPainter::SmoothPixmapTransform, true);
		if (mode == QLatin1String("tile")) {
			p.drawTiledPixmap(rect(), pix);
			return;
		}
		if (mode == QLatin1String("fill")) {
			/* cover the whole spot, cropping the overflow evenly */
			const qreal s = qMax((qreal)width() / pix.width(), (qreal)height() / pix.height());
			const qreal sw = width() / s, sh = height() / s;
			p.drawPixmap(rect(), pix, QRectF((pix.width() - sw) / 2.0, (pix.height() - sh) / 2.0, sw, sh));
			return;
		}
		/* fit: the whole picture, centered */
		const QSize target = pix.size().scaled(size(), Qt::KeepAspectRatio);
		p.drawPixmap(QRect(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target),
			     pix);
	}

#ifdef _WIN32
	QTimer *timer = nullptr;
	HWND hwnd = nullptr;
	bool topmostApplied = false;
	LONG_PTR origStyle = 0;
	bool styleStripped = false;
	LONG_PTR origOwner = 0;
	bool owned = false;
	bool ownershipVetoed = false; /* owned moves proved ineffective; stay on-top */
	int ownedMoveFails = 0;
	bool warnedMoveFail = false;
	int minForgetTicks = 0; /* countdown to forgetting a learned window minimum */

	/* seamless look: drop the pinned window's title bar + sizing frame
	   while pinned so it reads as pure content; the original style is
	   put back on unpin/remove/shutdown (and the app itself restores it
	   on its next restart, so nothing can stick permanently) */
	void applySeamless(bool want)
	{
		if (!hwnd || !IsWindow(hwnd))
			return;
		if (want && !styleStripped) {
			origStyle = GetWindowLongPtr(hwnd, GWL_STYLE);
			SetWindowLongPtr(hwnd, GWL_STYLE, origStyle & ~((LONG_PTR)(WS_CAPTION | WS_THICKFRAME)));
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			styleStripped = true;
		} else if (!want && styleStripped) {
			SetWindowLongPtr(hwnd, GWL_STYLE, origStyle);
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
			styleStripped = false;
		}
	}

	void dropTopmost()
	{
		if (topmostApplied && hwnd && IsWindow(hwnd)) {
			SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			topmostApplied = false;
		}
	}

	/* stacking: registering the pinned window as OWNED by the OBS main
	   window makes the shell stack it as part of OBS -- always just above
	   OBS, underneath whatever app the user actually selects, hidden with
	   OBS when OBS minimizes. This is what makes it FEEL native; the first
	   always-on-top approach sandwiched other apps between the pinned
	   window and OBS (Joey's 09-04 bug report). Cross-process owner
	   changes don't stick for elevated targets, so verify -- if it fails,
	   fall back to on-top-while-engaged */
	void applyOwnership()
	{
		if (owned || ownershipVetoed || !hwnd || !IsWindow(hwnd))
			return;
		QMainWindow *m = mainWindow();
		if (!m)
			return;
		const LONG_PTR obsWin = (LONG_PTR)m->winId();
		origOwner = GetWindowLongPtr(hwnd, GWLP_HWNDPARENT);
		if (origOwner == obsWin) {
			owned = true;
			return;
		}
		SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, obsWin);
		owned = GetWindowLongPtr(hwnd, GWLP_HWNDPARENT) == obsWin;
		if (owned) {
			dropTopmost(); /* the shell owns stacking now */
			/* changing the owner does NOT re-stack an already visible
			   window; without this lift, a window pinned while OBS is
			   focused lands BEHIND OBS and looks like nothing happened
			   (Joey's taste-test) */
			SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
			obs_log(LOG_INFO, "placeholder %d: window joined OBS's window group", id);
		} else {
			obs_log(LOG_INFO, "placeholder %d: window refused OBS ownership, using on-top fallback", id);
		}
	}

	void releaseOwnership()
	{
		if (owned && hwnd && IsWindow(hwnd)) {
			SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, origOwner);
			SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
				     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		}
		owned = false;
	}

	void tick()
	{
		PlaceholderEntry *e = entryFor(id);
		if (!e || e->pinTitle.isEmpty()) {
			releaseTarget();
			return;
		}
		if (hwnd && !IsWindow(hwnd)) {
			hwnd = nullptr;
			topmostApplied = false;
			styleStripped = false; /* a recreated window has its frame back */
			owned = false;
			ownershipVetoed = false;
			ownedMoveFails = 0;
			setMinimumSize(80, 60);
			update(); /* hint flips to the waiting message */
		}
		/* OBS minimized: an owned window hides with it automatically
		   (part of OBS, like its own dialogs); nothing to do */
		QWidget *top = window();
		if (top && top->isMinimized()) {
			if (hwnd && !owned)
				dropTopmost();
			return;
		}

		/* dock hidden (closed, tabbed behind, a layout without it):
		   the spot is gone, tuck the window away until it comes back
		   (Joey's call 09-04) */
		if (!isVisible()) {
			if (hwnd) {
				dropTopmost();
				if (!IsIconic(hwnd))
					ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
			}
			return;
		}

		RECT mine;
		if (!GetWindowRect(reinterpret_cast<HWND>(winId()), &mine))
			return;

		if (!hwnd) {
			hwnd = findPinTarget(e->pinTitle, e->pinExe, e->pinW, e->pinH, (int)(mine.right - mine.left),
					     (int)(mine.bottom - mine.top));
			if (hwnd)
				update(); /* hint flips from waiting to pinned */
		}
		if (!hwnd)
			return; /* e.g. the app re-docked its panel; wait for it */

		applySeamless(e->seamless);
		applyOwnership();

		/* a learned minimum must not outlive its reason: the app's own
		   minimum SHRINKS when the user trims panels inside it (Joey's
		   TikTok gift panel stuck the dock large). Every ~15s PROBE it:
		   nudge the window 2px smaller and put it right back. If it
		   obeyed, the old minimum is stale -- forget it; if it refused,
		   the minimum is still real -- keep it. The earlier version
		   forgot blindly, and when a layout squeezed this dock below
		   the real minimum Qt shrank the dock on every forget and the
		   relearn grew it again, so the whole layout visibly breathed
		   every 15s until it settled (Joey's shifting-layout report) */
		if (++minForgetTicks >= 60) {
			minForgetTicks = 0;
			if (minimumWidth() > 80 || minimumHeight() > 60) {
				RECT cw = {};
				if (GetWindowRect(hwnd, &cw)) {
					const int curW = (int)(cw.right - cw.left);
					const int curH = (int)(cw.bottom - cw.top);
					SetWindowPos(hwnd, nullptr, 0, 0, curW - 2, curH - 2,
						     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
					RECT probe = {};
					GetWindowRect(hwnd, &probe);
					const bool obeyed = (int)(probe.right - probe.left) < curW ||
							    (int)(probe.bottom - probe.top) < curH;
					SetWindowPos(hwnd, nullptr, 0, 0, curW, curH,
						     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
					if (obeyed)
						setMinimumSize(80, 60);
				}
			}
		}

		/* the spot is visible: the window belongs in it, even if
		   something minimized it meanwhile. A MAXIMIZED window ignores
		   resize requests, so bring it to normal first */
		if (IsIconic(hwnd) || IsZoomed(hwnd))
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);

		RECT theirs = {};
		GetWindowRect(hwnd, &theirs);
		const bool fits = abs((int)theirs.left - (int)mine.left) <= 1 &&
				  abs((int)theirs.top - (int)mine.top) <= 1 &&
				  abs((int)(theirs.right - theirs.left) - (int)(mine.right - mine.left)) <= 1 &&
				  abs((int)(theirs.bottom - theirs.top) - (int)(mine.bottom - mine.top)) <= 1;

		if (owned) {
			/* the shell keeps it stacked with OBS across activation
			   changes; we track the spot, and while the user is IN
			   OBS we also lift it back above OBS (repositioning can
			   happen right after adoption, before any activation has
			   let the shell enforce the owned-above-owner order) */
			if (fits)
				return;
			HWND fg = GetForegroundWindow();
			DWORD fgPid = 0;
			if (fg)
				GetWindowThreadProcessId(fg, &fgPid);
			const UINT zflag = (fg && fgPid == GetCurrentProcessId()) ? 0 : SWP_NOZORDER;
			const BOOL ok = SetWindowPos(hwnd, HWND_TOP, mine.left, mine.top, mine.right - mine.left,
						     mine.bottom - mine.top, SWP_NOACTIVATE | zflag);
			const DWORD err = GetLastError();

			/* trust nothing: verify the window actually went where we
			   sent it. If owned-mode repositioning has no effect on
			   this window (Joey hit a silent no-op pin), give up on
			   ownership and go back to the proven on-top approach */
			RECT check = {};
			GetWindowRect(hwnd, &check);
			const bool landed = abs((int)check.left - (int)mine.left) <= 8 &&
					    abs((int)check.top - (int)mine.top) <= 8;
			if (!ok || !landed) {
				if (++ownedMoveFails >= 3) {
					obs_log(LOG_WARNING,
						"placeholder %d: owned reposition not taking effect "
						"(ok=%d err=%lu at %ld,%ld want %ld,%ld), reverting to on-top mode",
						id, (int)ok, (unsigned long)err, (long)check.left, (long)check.top,
						(long)mine.left, (long)mine.top);
					releaseOwnership();
					ownershipVetoed = true;
					ownedMoveFails = 0;
				}
			} else {
				ownedMoveFails = 0;
			}
		} else {
			/* fallback for windows that refuse ownership: on top
			   only while OBS or the pinned app itself is in use */
			HWND fg = GetForegroundWindow();
			DWORD fgPid = 0, chatPid = 0;
			if (fg)
				GetWindowThreadProcessId(fg, &fgPid);
			GetWindowThreadProcessId(hwnd, &chatPid);
			const bool engaged = fg && (fgPid == GetCurrentProcessId() || (chatPid && fgPid == chatPid));
			if (!engaged) {
				dropTopmost();
				return;
			}
			if (fits && topmostApplied)
				return;
			if (!SetWindowPos(hwnd, HWND_TOPMOST, mine.left, mine.top, mine.right - mine.left,
					  mine.bottom - mine.top, SWP_NOACTIVATE)) {
				if (!warnedMoveFail) {
					warnedMoveFail = true;
					obs_log(LOG_WARNING,
						"placeholder %d: SetWindowPos failed (err %lu) -- "
						"target may be elevated; pinning cannot control it",
						id, (unsigned long)GetLastError());
				}
				return;
			}
			topmostApplied = true;
			if (fits)
				return; /* only the z order needed reasserting */
		}

		/* smart minimum: if the window refused to shrink to the spot
		   (it has its own minimum size), teach the placeholder that
		   minimum so the dock can never claim space the window can't
		   actually fit -- the layout stays honest */
		RECT after = {};
		if (GetWindowRect(hwnd, &after)) {
			const int gotW = (int)(after.right - after.left);
			const int gotH = (int)(after.bottom - after.top);
			const int wantW = (int)(mine.right - mine.left);
			const int wantH = (int)(mine.bottom - mine.top);
			const qreal dpr = devicePixelRatio() > 0 ? devicePixelRatio() : 1.0;
			int minW = minimumWidth(), minH = minimumHeight();
			if (gotW > wantW + 2)
				minW = qMax(minW, (int)std::ceil(gotW / dpr));
			if (gotH > wantH + 2)
				minH = qMax(minH, (int)std::ceil(gotH / dpr));
			if (minW != minimumWidth() || minH != minimumHeight())
				setMinimumSize(minW, minH);
		}
	}
#endif
};

static QList<PlaceholderPanel *> g_panels;

static PlaceholderPanel *panelFor(int id)
{
	for (PlaceholderPanel *p : g_panels)
		if (p->id == id)
			return p;
	return nullptr;
}

static PlaceholderPanel *registerPanel(const PlaceholderEntry &e)
{
	PlaceholderPanel *p = new PlaceholderPanel(e.id);
	if (!obs_frontend_add_dock_by_id(dockIdFor(e.id).toUtf8().constData(), titleFor(e).toUtf8().constData(), p)) {
		obs_log(LOG_WARNING, "could not register placeholder dock %d", e.id);
		delete p;
		return nullptr;
	}
	g_panels.push_back(p);
	return p;
}

/* ---------- public API ---------- */

void createFromState()
{
	for (const PlaceholderEntry &e : state().placeholders)
		registerPanel(e);
}

void addDock(const QString &label)
{
	PlaceholderEntry e;
	e.id = state().nextPlaceholderId++;
	e.label = label;
	state().placeholders.push_back(e);
	stateSave();
	if (!registerPanel(state().placeholders.back()))
		return;
	QMainWindow *m = mainWindow();
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(dockIdFor(e.id)) : nullptr;
	if (dock) {
		dock->setVisible(true);
		dock->raise();
	}
}

void removeDock(int id)
{
	for (auto it = g_panels.begin(); it != g_panels.end(); ++it) {
		if ((*it)->id == id) {
			(*it)->releaseTarget();
			g_panels.erase(it);
			break;
		}
	}
	obs_frontend_remove_dock(dockIdFor(id).toUtf8().constData());
	auto &v = state().placeholders;
	for (auto it = v.begin(); it != v.end(); ++it) {
		if (it->id == id) {
			v.erase(it);
			break;
		}
	}
	stateSave();
}

void setLabel(int id, const QString &label)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->label = label;
	stateSave();
	QMainWindow *m = mainWindow();
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(dockIdFor(id)) : nullptr;
	if (dock)
		dock->setWindowTitle(titleFor(*e));
	if (PlaceholderPanel *p = panelFor(id))
		p->update();
}

void setColor(int id, const QString &color)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->color = color;
	stateSave();
	if (PlaceholderPanel *p = panelFor(id))
		p->update();
}

bool pinningSupported()
{
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

void pinWindow(int id, QWidget *parent)
{
#ifdef _WIN32
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;

	const QList<QPair<QString, quintptr>> rows = listWindows();
	QDialog dlg(parent ? parent->window() : mainWindow());
	dlg.setWindowTitle("Pin a window");
	dlg.setMinimumSize(420, 360);
	QVBoxLayout *v = new QVBoxLayout(&dlg);
	QLabel *intro = new QLabel("Pick the window to keep on top of OBS, sized exactly over this "
				   "placeholder. It follows the placeholder wherever you dock it. "
				   "When an app's popped out panel shares a name with the app itself, "
				   "the size at the end of the row tells them apart: pin the SMALL "
				   "one (the panel, not the whole app). Unpin any time from the "
				   "placeholder's right click menu.",
				   &dlg);
	intro->setWordWrap(true);
	v->addWidget(intro);
	QListWidget *list = new QListWidget(&dlg);
	for (const auto &row : rows) {
		/* exe + size make twin titles tellable apart: an app's popped
		   out panel and its main window often share a name, but never
		   a size */
		QString shown = row.first;
		const QString exe = exeBaseName(reinterpret_cast<HWND>(row.second));
		if (!exe.isEmpty())
			shown += QString(" · %1").arg(exe);
		RECT r = {};
		if (GetWindowRect(reinterpret_cast<HWND>(row.second), &r))
			shown += QString(" · %1×%2").arg(r.right - r.left).arg(r.bottom - r.top);
		QListWidgetItem *it = new QListWidgetItem(shown, list);
		it->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(row.second));
		it->setData(Qt::UserRole + 1, row.first); /* the real title */
	}
	v->addWidget(list, 1);
	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
	bb->button(QDialogButtonBox::Ok)->setText("Pin window");
	QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
	QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&dlg](QListWidgetItem *) { dlg.accept(); });
	v->addWidget(bb);

	if (dlg.exec() != QDialog::Accepted)
		return;
	QListWidgetItem *it = list->currentItem();
	if (!it)
		return;

	const HWND picked = reinterpret_cast<HWND>((quintptr)it->data(Qt::UserRole).toULongLong());
	e->pinTitle = it->data(Qt::UserRole + 1).toString();
	e->pinExe = exeBaseName(picked);
	/* remember the window's size: on re-find it is the strongest tell
	   between this panel and the app's main window (titles often match) */
	e->pinW = e->pinH = 0;
	RECT pr = {};
	if (GetWindowRect(picked, &pr)) {
		e->pinW = (int)(pr.right - pr.left);
		e->pinH = (int)(pr.bottom - pr.top);
	}
	if (!e->mediaPath.isEmpty()) {
		e->mediaPath.clear(); /* one spot, one occupant */
		if (PlaceholderPanel *p = panelFor(id))
			p->refreshMedia();
	}
	stateSave();
	if (PlaceholderPanel *p = panelFor(id)) {
		p->adoptTarget((quintptr)it->data(Qt::UserRole).toULongLong());
		p->update();
	}
	obs_log(LOG_INFO, "placeholder %d pinned window \"%s\" (%s, %dx%d)", id, e->pinTitle.toUtf8().constData(),
		e->pinExe.toUtf8().constData(), e->pinW, e->pinH);
#else
	(void)id;
	(void)parent;
#endif
}

void setSeamless(int id, bool on)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->seamless = on;
	stateSave();
	if (PlaceholderPanel *p = panelFor(id))
		p->update(); /* the follower applies the style on its next tick */
}

void unpinWindow(int id)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->pinTitle.clear();
	e->pinExe.clear();
	e->pinW = e->pinH = 0;
	stateSave();
	if (PlaceholderPanel *p = panelFor(id)) {
		p->releaseTarget();
		p->update();
	}
}

void chooseMedia(int id, QWidget *parent)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	const QString path = QFileDialog::getOpenFileName(
		parent ? parent->window() : mainWindow(), "Show an image or video in this spot", QString(),
		"Images and videos (*.png *.jpg *.jpeg *.bmp *.webp *.svg *.gif *.mp4 *.mov *.mkv *.webm *.avi "
		"*.m4v);;All files (*)");
	if (path.isEmpty())
		return;
	if (!e->pinTitle.isEmpty())
		unpinWindow(id); /* one spot, one occupant */
	e = entryFor(id);
	if (!e)
		return;
	e->mediaPath = path;
	if (e->mediaMode.isEmpty())
		e->mediaMode = QStringLiteral("fit");
	stateSave();
	if (PlaceholderPanel *p = panelFor(id))
		p->refreshMedia();
	obs_log(LOG_INFO, "placeholder %d shows \"%s\"", id, path.toUtf8().constData());
}

void clearMedia(int id)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->mediaPath.clear();
	stateSave();
	if (PlaceholderPanel *p = panelFor(id))
		p->refreshMedia();
}

void setMediaMode(int id, const QString &mode)
{
	PlaceholderEntry *e = entryFor(id);
	if (!e)
		return;
	e->mediaMode = mode;
	stateSave();
	if (PlaceholderPanel *p = panelFor(id))
		p->refreshMedia();
}

void shutdown()
{
	for (PlaceholderPanel *p : g_panels) {
		p->releaseTarget();
		p->releaseMedia(); /* displays + the private player die while OBS is still up */
	}
	g_panels.clear();
}

} // namespace placeholders
} // namespace dockx
