/*
DockX for OBS Studio (by StrmrX) -- placeholder docks.
GPL v2, see plugin-main.cpp for the full notice.

An empty labeled dock that reserves a spot in the layout for a window OBS
cannot host (TikTok Live Studio chat, a music player, any app floated over
OBS). On Windows that window can be PINNED: it becomes an OWNED window of
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
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

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

struct FindCtx {
	QString wanted;
	HWND exact = nullptr;
	HWND partial = nullptr;
};

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

static BOOL CALLBACK findByTitleCb(HWND h, LPARAM lp)
{
	FindCtx *ctx = reinterpret_cast<FindCtx *>(lp);
	if (!eligibleWindow(h))
		return TRUE;
	const QString t = windowTitle(h);
	if (t == ctx->wanted) {
		ctx->exact = h;
		return FALSE;
	}
	if (!ctx->partial && t.contains(ctx->wanted, Qt::CaseInsensitive))
		ctx->partial = h;
	return TRUE;
}

static HWND findByTitle(const QString &title)
{
	FindCtx ctx;
	ctx.wanted = title;
	EnumWindows(findByTitleCb, reinterpret_cast<LPARAM>(&ctx));
	return ctx.exact ? ctx.exact : ctx.partial;
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
	}

	~PlaceholderPanel() override { releaseTarget(); }

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
		if (pinned)
			hint = QString("Pinned: %1").arg(e->pinTitle);
		else if (pinningSupported())
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
			}
		} else {
			QAction *na = menu.addAction("Window pinning: Windows only for now");
			na->setEnabled(false);
		}

		menu.addSeparator();
		QObject::connect(menu.addAction("Remove this placeholder"), &QAction::triggered, this,
				 [this]() { QTimer::singleShot(0, [pid = id]() { removeDock(pid); }); });

		menu.exec(ev->globalPos());
	}

private:
#ifdef _WIN32
	QTimer *timer = nullptr;
	HWND hwnd = nullptr;
	bool topmostApplied = false;
	LONG_PTR origStyle = 0;
	bool styleStripped = false;
	LONG_PTR origOwner = 0;
	bool owned = false;

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
		if (owned || !hwnd || !IsWindow(hwnd))
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
		if (owned)
			dropTopmost(); /* the shell owns stacking now */
		else
			obs_log(LOG_INFO, "placeholder %d: window refused OBS ownership, using on-top fallback", id);
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
			setMinimumSize(80, 60);
		}
		if (!hwnd)
			hwnd = findByTitle(e->pinTitle);
		if (!hwnd)
			return;

		applySeamless(e->seamless);
		applyOwnership();

		/* OBS minimized: an owned window hides with it automatically
		   (part of OBS, like its own dialogs); nothing to do */
		QWidget *top = window();
		if (top && top->isMinimized()) {
			if (!owned)
				dropTopmost();
			return;
		}

		/* dock hidden (closed, tabbed behind, a layout without it):
		   the spot is gone, tuck the window away until it comes back
		   (Joey's call 09-04) */
		if (!isVisible()) {
			dropTopmost();
			if (!IsIconic(hwnd))
				ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
			return;
		}

		/* the spot is visible: the window belongs in it, even if
		   something minimized it meanwhile */
		if (IsIconic(hwnd))
			ShowWindow(hwnd, SW_SHOWNOACTIVATE);

		RECT mine;
		if (!GetWindowRect(reinterpret_cast<HWND>(winId()), &mine))
			return;
		RECT theirs = {};
		GetWindowRect(hwnd, &theirs);
		const bool fits = abs((int)theirs.left - (int)mine.left) <= 1 &&
				  abs((int)theirs.top - (int)mine.top) <= 1 &&
				  abs((int)(theirs.right - theirs.left) - (int)(mine.right - mine.left)) <= 1 &&
				  abs((int)(theirs.bottom - theirs.top) - (int)(mine.bottom - mine.top)) <= 1;

		if (owned) {
			/* the shell keeps it stacked with OBS; we only track the
			   spot */
			if (fits)
				return;
			SetWindowPos(hwnd, nullptr, mine.left, mine.top, mine.right - mine.left, mine.bottom - mine.top,
				     SWP_NOZORDER | SWP_NOACTIVATE);
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
			SetWindowPos(hwnd, HWND_TOPMOST, mine.left, mine.top, mine.right - mine.left,
				     mine.bottom - mine.top, SWP_NOACTIVATE);
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
				   "Unpin any time from the placeholder's right click menu.",
				   &dlg);
	intro->setWordWrap(true);
	v->addWidget(intro);
	QListWidget *list = new QListWidget(&dlg);
	for (const auto &row : rows) {
		QListWidgetItem *it = new QListWidgetItem(row.first, list);
		it->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(row.second));
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

	e->pinTitle = it->text();
	stateSave();
	if (PlaceholderPanel *p = panelFor(id)) {
		p->adoptTarget((quintptr)it->data(Qt::UserRole).toULongLong());
		p->update();
	}
	obs_log(LOG_INFO, "placeholder %d pinned window \"%s\"", id, it->text().toUtf8().constData());
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
	stateSave();
	if (PlaceholderPanel *p = panelFor(id)) {
		p->releaseTarget();
		p->update();
	}
}

void shutdown()
{
	for (PlaceholderPanel *p : g_panels)
		p->releaseTarget();
	g_panels.clear();
}

} // namespace placeholders
} // namespace dockx
