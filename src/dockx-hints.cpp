/*
DockX for OBS Studio (by StrmrX) -- the dock layout guide.
GPL v2, see plugin-main.cpp for the full notice.

A small illustrated walkthrough of OBS dock dragging: grab the title bar,
edge drops split, center drops make tabs, and the column layouts DockX
unlocks. Shown once on first run (drag_hints_shown flag); reopenable from
the Settings tab and linked from the Scene Folders dock help button.
Diagrams are painted from the palette so they match any OBS theme.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

namespace dockx {

const char *HELP_URL = "https://strmrx.com/dockx";

namespace hints {

/* one small painted diagram per hint row */
class HintArt : public QWidget {
public:
	enum Kind { Move, Edge, Center, Columns };

	HintArt(Kind k, QWidget *parent) : QWidget(parent), kind(k) { setFixedSize(150, 96); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);

		const QColor frame = palette().color(QPalette::Mid);
		const QColor dock = palette().color(QPalette::Base);
		const QColor title = palette().color(QPalette::Window);
		const QColor accent = palette().color(QPalette::Highlight);
		const QColor text = palette().color(QPalette::WindowText);

		/* the OBS window */
		QRectF win(4, 4, width() - 8, height() - 8);
		p.setPen(QPen(frame, 1.5));
		p.setBrush(title);
		p.drawRoundedRect(win, 4, 4);

		auto drawDock = [&](QRectF r, bool hot, bool ghost) {
			QPen pen(hot ? accent : frame, ghost ? 1.0 : 1.2);
			if (ghost)
				pen.setStyle(Qt::DashLine);
			p.setPen(pen);
			p.setBrush(ghost ? Qt::NoBrush : QBrush(dock));
			p.drawRoundedRect(r, 3, 3);
			/* title bar strip */
			QRectF t(r.left(), r.top(), r.width(), 9);
			p.setBrush(hot ? accent : frame);
			if (!ghost)
				p.drawRoundedRect(t, 3, 3);
		};

		auto drawArrow = [&](QPointF from, QPointF to) {
			p.setPen(QPen(accent, 2));
			p.setBrush(accent);
			p.drawLine(from, to);
			QLineF l(to, from);
			l.setLength(6);
			QLineF a = l, b = l;
			a.setAngle(l.angle() + 28);
			b.setAngle(l.angle() - 28);
			QPolygonF head;
			head << to << a.p2() << b.p2();
			p.drawPolygon(head);
		};

		switch (kind) {
		case Move: {
			/* one dock, accent title bar, arrow showing the grab + drag */
			drawDock(QRectF(20, 26, 62, 52), true, false);
			drawArrow(QPointF(66, 30), QPointF(118, 22));
			break;
		}
		case Edge: {
			/* target dock right, ghost dock flying at its left edge */
			drawDock(QRectF(76, 14, 60, 68), false, false);
			/* the drop zone strip */
			p.setPen(Qt::NoPen);
			QColor zone = accent;
			zone.setAlpha(120);
			p.setBrush(zone);
			p.drawRoundedRect(QRectF(70, 14, 12, 68), 3, 3);
			drawDock(QRectF(14, 30, 40, 36), false, true);
			drawArrow(QPointF(56, 48), QPointF(72, 48));
			break;
		}
		case Center: {
			/* target dock with tabs, ghost dropping on its middle */
			QRectF target(66, 14, 70, 68);
			drawDock(target, false, false);
			/* two little tabs under the title bar */
			p.setPen(QPen(frame, 1));
			p.setBrush(dock);
			p.drawRoundedRect(QRectF(70, 25, 26, 8), 2, 2);
			p.setBrush(accent);
			p.drawRoundedRect(QRectF(98, 25, 26, 8), 2, 2);
			/* center drop zone */
			p.setPen(Qt::NoPen);
			QColor zone = accent;
			zone.setAlpha(90);
			p.setBrush(zone);
			p.drawRoundedRect(target.adjusted(16, 26, -16, -14), 3, 3);
			drawDock(QRectF(12, 34, 38, 34), false, true);
			drawArrow(QPointF(52, 51), QPointF(80, 51));
			break;
		}
		case Columns: {
			/* the DockX money shot: full height column + stacked column */
			drawDock(QRectF(12, 12, 38, 72), true, false);
			drawDock(QRectF(56, 12, 38, 34), false, false);
			drawDock(QRectF(56, 50, 38, 34), false, false);
			drawDock(QRectF(100, 12, 36, 34), false, false);
			drawDock(QRectF(100, 50, 36, 34), false, false);
			break;
		}
		}
		Q_UNUSED(text);
	}

private:
	Kind kind;
};

static void addHint(QVBoxLayout *v, QWidget *parent, HintArt::Kind kind, const QString &heading, const QString &body)
{
	QHBoxLayout *row = new QHBoxLayout();
	row->setSpacing(12);
	HintArt *art = new HintArt(kind, parent);
	row->addWidget(art, 0, Qt::AlignTop);
	QLabel *label = new QLabel(QString("<b>%1</b><br>%2").arg(heading, body), parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::RichText);
	row->addWidget(label, 1);
	v->addLayout(row);
}

void showGuide(QWidget *parent)
{
	QWidget *host = parent;
	if (!host)
		host = static_cast<QWidget *>(obs_frontend_get_main_window());

	QDialog *dlg = new QDialog(host);
	dlg->setAttribute(Qt::WA_DeleteOnClose);
	dlg->setWindowTitle("DockX · Dock Layout Guide");
	/* modal over the Tools dialog (which runs exec); free floating at first run */
	dlg->setWindowModality(parent ? Qt::WindowModal : Qt::NonModal);
	dlg->setMinimumWidth(540);

	QVBoxLayout *v = new QVBoxLayout(dlg);
	v->setSpacing(14);

	QLabel *intro = new QLabel("Every panel in OBS is a dock you can move. DockX unlocks the layouts "
				   "OBS normally refuses. The whole trick:",
				   dlg);
	intro->setWordWrap(true);
	v->addWidget(intro);

	addHint(v, dlg, HintArt::Move, "Grab the title bar",
		"Docks move by their title bar. Click it, hold, and drag. If nothing "
		"moves, make sure Docks &gt; Lock UI is off in the OBS menu.");
	addHint(v, dlg, HintArt::Edge, "Drop on an edge to split",
		"While dragging, aim for the edge of another dock or the window. A "
		"preview shows the split before you let go. This is how you make "
		"side by side columns.");
	addHint(v, dlg, HintArt::Center, "Drop on the middle to make tabs",
		"Aim for the center of another dock instead and they stack into "
		"tabs, sharing one spot. Great for panels you swap between.");
	addHint(v, dlg, HintArt::Columns, "Build real columns",
		"With DockX, columns can hold stacks of docks side by side: full "
		"height chat next to a column of tools finally works. Happy with a "
		"layout? Save it in Tools &gt; DockX and give it a hotkey.");

	QLabel *folders = new QLabel("<b>Tip:</b> the new Scene Folders dock does everything the OBS Scenes "
				     "panel does, plus folders, search, colors, and your sources under each "
				     "scene. Drop it where your Scenes panel sits and you get the upgrade "
				     "without losing anything.",
				     dlg);
	folders->setWordWrap(true);
	folders->setTextFormat(Qt::RichText);
	v->addWidget(folders);

	QLabel *optional = new QLabel("Prefer to keep things simple? DockX stays out of your way. Optional "
				      "extras, like Align &amp; distribute tools for lining sources up on the "
				      "canvas, start switched off. Turn on anything you want any time in "
				      "Tools &gt; DockX &gt; Settings.",
				      dlg);
	optional->setWordWrap(true);
	v->addWidget(optional);

	QLabel *more =
		new QLabel(QString("More help and guides: <a href=\"%1\">strmrx.com/dockx</a>").arg(HELP_URL), dlg);
	more->setOpenExternalLinks(true);
	v->addWidget(more);

	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addStretch(1);
	QPushButton *ok = new QPushButton("Got it", dlg);
	ok->setDefault(true);
	QObject::connect(ok, &QPushButton::clicked, dlg, [dlg]() { dlg->accept(); });
	bottom->addWidget(ok);
	v->addLayout(bottom);

	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

void showFirstRun()
{
	if (state().dragHintsShown)
		return;
	state().dragHintsShown = true;
	stateSave();
	/* let OBS finish painting itself before we introduce ourselves */
	QTimer::singleShot(1200, []() { showGuide(nullptr); });
}

} // namespace hints
} // namespace dockx
