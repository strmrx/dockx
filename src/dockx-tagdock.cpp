/*
DockX for OBS Studio (by StrmrX) -- the DockX Tags dock.
GPL v2, see plugin-main.cpp for the full notice.

A panel that lives inside OBS and lists every tag with one-click Hide / Show for
the whole tag across every scene (Lock / Unlock / Mute / Unmute on right-click).
This is the clean in-OBS home for tag actions: OBS builds its own source
right-click menu and a plugin cannot safely add to it without replacing the whole
thing, so DockX gives tags their own dockable surface instead. Registered at load,
hidden until opened from the Tags tab.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QBoxLayout>
#include <QDockWidget>
#include <QFrame>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QWidget>

namespace dockx {
namespace tagdock {

static const char *DOCK_ID = "dockx_tags_panel";

namespace {

class TagPanel : public QWidget {
public:
	explicit TagPanel(QWidget *parent = nullptr) : QWidget(parent)
	{
		QVBoxLayout *outer = new QVBoxLayout(this);
		outer->setContentsMargins(8, 8, 8, 8);
		outer->setSpacing(6);

		QLabel *head = new QLabel(
			"Hide or show a whole tag across every scene. Right-click a tag for Lock and Mute.", this);
		head->setWordWrap(true);
		outer->addWidget(head);

		QScrollArea *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		rowsHost = new QWidget(scroll);
		rows = new QVBoxLayout(rowsHost);
		rows->setContentsMargins(0, 0, 0, 0);
		rows->setSpacing(4);
		rows->addStretch(1);
		scroll->setWidget(rowsHost);
		outer->addWidget(scroll, 1);

		rebuild();
	}

	void rebuild()
	{
		/* clear every existing row/label, then re-add the trailing stretch */
		QLayoutItem *item;
		while ((item = rows->takeAt(0)) != nullptr) {
			if (QWidget *w = item->widget())
				w->deleteLater();
			delete item;
		}

		const QStringList allTags = tags::allTags();
		if (allTags.isEmpty()) {
			QLabel *empty = new QLabel("No tags yet. Add tags in Tools menu, DockX, Tags.", rowsHost);
			empty->setWordWrap(true);
			empty->setEnabled(false);
			rows->addWidget(empty);
			rows->addStretch(1);
			return;
		}

		for (const QString &t : allTags) {
			QWidget *row = new QWidget(rowsHost);
			QHBoxLayout *h = new QHBoxLayout(row);
			h->setContentsMargins(0, 0, 0, 0);
			h->setSpacing(6);

			QLabel *name = new QLabel(t, row);
			name->setToolTip(t);
			h->addWidget(name, 1);

			QPushButton *hide = new QPushButton("Hide", row);
			QPushButton *show = new QPushButton("Show", row);
			hide->setToolTip(QString("Hide every source tagged \"%1\", in every scene").arg(t));
			show->setToolTip(QString("Show every source tagged \"%1\", in every scene").arg(t));
			QObject::connect(hide, &QPushButton::clicked, row,
					 [t]() { tags::applyBulkByTag(t, tags::HIDE); });
			QObject::connect(show, &QPushButton::clicked, row,
					 [t]() { tags::applyBulkByTag(t, tags::SHOW); });
			h->addWidget(hide);
			h->addWidget(show);

			row->setContextMenuPolicy(Qt::CustomContextMenu);
			QObject::connect(row, &QWidget::customContextMenuRequested, row, [row, t](const QPoint &p) {
				QMenu menu(row);
				QObject::connect(menu.addAction(QString("Lock all \"%1\"").arg(t)),
						 &QAction::triggered, row,
						 [t]() { tags::applyBulkByTag(t, tags::LOCK); });
				QObject::connect(menu.addAction(QString("Unlock all \"%1\"").arg(t)),
						 &QAction::triggered, row,
						 [t]() { tags::applyBulkByTag(t, tags::UNLOCK); });
				menu.addSeparator();
				QObject::connect(menu.addAction(QString("Mute \"%1\"").arg(t)), &QAction::triggered,
						 row, [t]() { tags::applyBulkByTag(t, tags::MUTE); });
				QObject::connect(menu.addAction(QString("Unmute \"%1\"").arg(t)), &QAction::triggered,
						 row, [t]() { tags::applyBulkByTag(t, tags::UNMUTE); });
				menu.exec(row->mapToGlobal(p));
			});

			rows->addWidget(row);
		}
		rows->addStretch(1);
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		rebuild(); /* tags can change (edits, scene-collection switch) while hidden */
	}

private:
	QWidget *rowsHost = nullptr;
	QVBoxLayout *rows = nullptr;
};

} // namespace

static QPointer<TagPanel> g_panel;

void createDock()
{
	if (g_panel)
		return;
	TagPanel *p = new TagPanel();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, "DockX Tags", p)) {
		obs_log(LOG_WARNING, "could not register the DockX Tags dock");
		delete p;
		return;
	}
	g_panel = p;
}

void showDock()
{
	QMainWindow *m = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	QDockWidget *dock = m ? m->findChild<QDockWidget *>(DOCK_ID) : nullptr;
	if (!dock)
		return;
	if (g_panel)
		g_panel->rebuild();
	dock->setVisible(true);
	dock->raise();
}

void refresh()
{
	if (g_panel)
		g_panel->rebuild();
}

void shutdown()
{
	/* OBS owns and destroys the dock widget at exit; just drop our handle */
	g_panel = nullptr;
}

} // namespace tagdock
} // namespace dockx
