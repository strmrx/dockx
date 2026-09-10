/*
DockX for OBS Studio (by StrmrX) -- the Tools menu dialog.
GPL v2, see plugin-main.cpp for the full notice.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAbstractItemView>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFont>
#include <QUrl>
#include <QStandardItemModel>
#include <QDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QFileInfo>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTabBar>
#include <QScreen>
#include <QGuiApplication>
#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QPainter>
#include <QFrame>

#include <functional>
#include <memory>

namespace dockx {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* a list that explains itself while it is empty: the guidance lives IN the
   empty space instead of a paragraph below, and disappears once there are
   rows (part of the dialog de-wording pass, Joey 2026-09-09) */
class HintList : public QListWidget {
public:
	HintList(const QString &hint, QWidget *parent = nullptr) : QListWidget(parent), hintText(hint)
	{
		setSpacing(2);
	}

protected:
	void paintEvent(QPaintEvent *ev) override
	{
		QListWidget::paintEvent(ev);
		if (count() > 0)
			return;
		QPainter p(viewport());
		QColor c = palette().color(QPalette::Text);
		c.setAlpha(150);
		p.setPen(c);
		p.drawText(viewport()->rect().adjusted(24, 12, -24, -12), Qt::AlignCenter | Qt::TextWordWrap, hintText);
	}

private:
	QString hintText;
};

/* a quiet one line subtitle under a group title; carries the whole
   explanation so the boxes need no paragraph of small print */
static QLabel *groupSub(const QString &text, QWidget *parent)
{
	QLabel *l = new QLabel(text, parent);
	l->setWordWrap(true);
	QColor c = l->palette().color(QPalette::Text);
	l->setStyleSheet(QString("color: rgba(%1,%2,%3,165);").arg(c.red()).arg(c.green()).arg(c.blue()));
	return l;
}

/* a small stripe swatch of a look's palette, for the looks dropdown */
static QIcon paletteIcon(const QStringList &cols)
{
	QPixmap pm(20, 16);
	pm.fill(Qt::transparent);
	QPainter p(&pm);
	const int w = 20 / qMax(1, (int)cols.size());
	for (int i = 0; i < (int)cols.size(); i++)
		p.fillRect(i * w, 2, w, 12, QColor(cols.at(i)));
	return QIcon(pm);
}

/* ONE accent styled button per box: the action the box exists for */
static void makePrimary(QPushButton *b)
{
	b->setStyleSheet("QPushButton { background-color: #8c1eff; color: #ffffff; font-weight: 600;"
			 " border: none; border-radius: 4px; padding: 5px 14px; }"
			 " QPushButton:hover { background-color: #9d43ff; }"
			 " QPushButton:pressed { background-color: #7a14e0; }"
			 " QPushButton:disabled { background-color: rgba(140,30,255,90);"
			 " color: rgba(255,255,255,140); }");
}

static QStringList sceneNames()
{
	QStringList out;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++)
		out << QString::fromUtf8(obs_source_get_name(list.sources.array[i]));
	obs_frontend_source_list_free(&list);
	return out;
}

struct SceneRef {
	QString uuid;
	QString name;
};

static QList<SceneRef> sceneRefs()
{
	QList<SceneRef> out;
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		SceneRef r;
		r.uuid = QString::fromUtf8(obs_source_get_uuid(list.sources.array[i]));
		r.name = QString::fromUtf8(obs_source_get_name(list.sources.array[i]));
		out << r;
	}
	obs_frontend_source_list_free(&list);
	return out;
}

static bool collectDockableInput(void *param, obs_source_t *src)
{
	auto *out = static_cast<QStringList *>(param);
	if (obs_source_get_output_flags(src) & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)) {
		const char *n = obs_source_get_name(src);
		if (n && *n)
			*out << QString::fromUtf8(n);
	}
	return true;
}

static QStringList dockableInputNames()
{
	QStringList out;
	obs_enum_sources(collectDockableInput, &out);
	out.sort(Qt::CaseInsensitive);
	return out;
}

static bool collectAudioInput(void *param, obs_source_t *src)
{
	auto *out = static_cast<QStringList *>(param);
	if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
		const char *n = obs_source_get_name(src);
		if (n && *n)
			*out << QString::fromUtf8(n);
	}
	return true;
}

static QStringList audioInputNames()
{
	QStringList out;
	obs_enum_sources(collectAudioInput, &out);
	out.sort(Qt::CaseInsensitive);
	return out;
}

/* one row of preset swatches + Custom + No color, calling apply(hex) */
static void addPaletteRow(QWidget *tab, QVBoxLayout *v, QDialog *dlg, std::function<void(const QString &)> apply)
{
	QHBoxLayout *row = new QHBoxLayout();
	for (const char *hex : PRESET_COLORS) {
		QPushButton *b = new QPushButton(tab);
		b->setFixedSize(26, 26);
		b->setStyleSheet(QString("background:%1; border-radius:5px; border:none;").arg(hex));
		b->setToolTip("Use this color");
		QString h = QString::fromUtf8(hex);
		QObject::connect(b, &QPushButton::clicked, tab, [apply, h]() { apply(h); });
		row->addWidget(b);
	}
	QPushButton *customBtn = new QPushButton("Custom", tab);
	QObject::connect(customBtn, &QPushButton::clicked, dlg, [dlg, apply]() {
		QColor c = QColorDialog::getColor(Qt::white, dlg, "Pick a color");
		if (c.isValid())
			apply(c.name());
	});
	QPushButton *noneBtn = new QPushButton("No color", tab);
	QObject::connect(noneBtn, &QPushButton::clicked, tab, [apply]() { apply(QString()); });
	row->addWidget(customBtn);
	row->addWidget(noneBtn);
	row->addStretch(1);
	v->addLayout(row);
}

/* ---- hotkey binding from inside the dialog ---- */

static QString qtKeyToObsName(int key)
{
	if (key >= Qt::Key_A && key <= Qt::Key_Z)
		return QString("OBS_KEY_%1").arg(QChar((char)('A' + (key - Qt::Key_A))));
	if (key >= Qt::Key_0 && key <= Qt::Key_9)
		return QString("OBS_KEY_%1").arg(key - Qt::Key_0);
	if (key >= Qt::Key_F1 && key <= Qt::Key_F24)
		return QString("OBS_KEY_F%1").arg(key - Qt::Key_F1 + 1);
	switch (key) {
	case Qt::Key_Space:
		return "OBS_KEY_SPACE";
	case Qt::Key_Return:
		return "OBS_KEY_RETURN";
	case Qt::Key_Enter:
		return "OBS_KEY_ENTER";
	case Qt::Key_Insert:
		return "OBS_KEY_INSERT";
	case Qt::Key_Delete:
		return "OBS_KEY_DELETE";
	case Qt::Key_Home:
		return "OBS_KEY_HOME";
	case Qt::Key_End:
		return "OBS_KEY_END";
	case Qt::Key_PageUp:
		return "OBS_KEY_PAGEUP";
	case Qt::Key_PageDown:
		return "OBS_KEY_PAGEDOWN";
	case Qt::Key_Left:
		return "OBS_KEY_LEFT";
	case Qt::Key_Right:
		return "OBS_KEY_RIGHT";
	case Qt::Key_Up:
		return "OBS_KEY_UP";
	case Qt::Key_Down:
		return "OBS_KEY_DOWN";
	case Qt::Key_Comma:
		return "OBS_KEY_COMMA";
	case Qt::Key_Period:
		return "OBS_KEY_PERIOD";
	case Qt::Key_Semicolon:
		return "OBS_KEY_SEMICOLON";
	case Qt::Key_Apostrophe:
		return "OBS_KEY_APOSTROPHE";
	case Qt::Key_BracketLeft:
		return "OBS_KEY_BRACKETLEFT";
	case Qt::Key_BracketRight:
		return "OBS_KEY_BRACKETRIGHT";
	case Qt::Key_Backslash:
		return "OBS_KEY_BACKSLASH";
	case Qt::Key_Slash:
		return "OBS_KEY_SLASH";
	case Qt::Key_Minus:
		return "OBS_KEY_MINUS";
	case Qt::Key_Equal:
		return "OBS_KEY_EQUAL";
	default:
		return QString();
	}
}

/* empty sequence removes the binding; returns false if the key is unsupported */
static bool applyHotkeyBinding(obs_hotkey_id id, const QKeySequence &seq)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return false;
	obs_data_array_t *arr = obs_data_array_create();
	if (!seq.isEmpty()) {
		const QKeyCombination combo = seq[0];
		const QString name = qtKeyToObsName(combo.key());
		if (name.isEmpty() || obs_key_from_name(name.toUtf8().constData()) == OBS_KEY_NONE) {
			obs_data_array_release(arr);
			return false;
		}
		obs_data_t *b = obs_data_create();
		obs_data_set_string(b, "key", name.toUtf8().constData());
		obs_data_set_bool(b, "shift", combo.keyboardModifiers() & Qt::ShiftModifier);
		obs_data_set_bool(b, "control", combo.keyboardModifiers() & Qt::ControlModifier);
		obs_data_set_bool(b, "alt", combo.keyboardModifiers() & Qt::AltModifier);
		obs_data_set_bool(b, "command", combo.keyboardModifiers() & Qt::MetaModifier);
		obs_data_array_push_back(arr, b);
		obs_data_release(b);
	}
	obs_hotkey_load(id, arr);
	obs_data_array_release(arr);
	return true;
}

static bool setLayoutHotkey(Layout *l, const QKeySequence &seq)
{
	if (!l || !applyHotkeyBinding(l->hotkey, seq))
		return false;
	stateSave();
	return true;
}

static QString hotkeyIdText(obs_hotkey_id id)
{
	if (id == OBS_INVALID_HOTKEY_ID)
		return QString();
	obs_data_array_t *arr = obs_hotkey_save(id);
	if (!arr)
		return QString();
	QString out;
	if (obs_data_array_count(arr) > 0) {
		obs_data_t *b = obs_data_array_item(arr, 0);
		QString key = QString::fromUtf8(obs_data_get_string(b, "key"));
		key.remove(QStringLiteral("OBS_KEY_"));
		QStringList parts;
		if (obs_data_get_bool(b, "control"))
			parts << "Ctrl";
		if (obs_data_get_bool(b, "alt"))
			parts << "Alt";
		if (obs_data_get_bool(b, "shift"))
			parts << "Shift";
		if (obs_data_get_bool(b, "command"))
			parts << "Meta";
		parts << key;
		out = parts.join("+");
		obs_data_release(b);
	}
	obs_data_array_release(arr);
	return out;
}

static QString layoutHotkeyText(const Layout &l)
{
	return hotkeyIdText(l.hotkey);
}

/* shared key-capture popup; returns 0 = cancel, 1 = save (seq filled), 2 = remove */
static int promptHotkey(QWidget *parent, const QString &title, QKeySequence &seq)
{
	QDialog hd(parent);
	hd.setWindowTitle(title);
	QVBoxLayout *v = new QVBoxLayout(&hd);
	v->addWidget(new QLabel("Press the keys you want:", &hd));
	QKeySequenceEdit *edit = new QKeySequenceEdit(&hd);
	v->addWidget(edit);
	QHBoxLayout *hb = new QHBoxLayout();
	QPushButton *ok = new QPushButton("Save", &hd);
	QPushButton *clear = new QPushButton("Remove hotkey", &hd);
	QPushButton *cancel = new QPushButton("Cancel", &hd);
	hb->addWidget(ok);
	hb->addWidget(clear);
	hb->addWidget(cancel);
	v->addLayout(hb);
	QObject::connect(ok, &QPushButton::clicked, &hd, [&hd]() { hd.done(1); });
	QObject::connect(clear, &QPushButton::clicked, &hd, [&hd]() { hd.done(2); });
	QObject::connect(cancel, &QPushButton::clicked, &hd, [&hd]() { hd.reject(); });
	edit->setFocus();
	const int r = hd.exec();
	if (r == 1) {
		seq = edit->keySequence();
		if (seq.isEmpty())
			return 0;
	}
	return r == QDialog::Rejected ? 0 : r;
}

void showDialog(const QString &initialTab)
{
	/* ONE non-modal window: OBS stays fully clickable while it is open, and
	   windows it spawns (Properties, pin pickers) come to the front instead
	   of popping up stuck behind a modal dialog (Joey's Find tab feedback) */
	static QPointer<QDialog> openDlg;
	if (openDlg) {
		if (!initialTab.isEmpty()) {
			if (QTabWidget *t = openDlg->findChild<QTabWidget *>())
				for (int i = 0; i < t->count(); i++)
					if (t->tabText(i) == initialTab)
						t->setCurrentIndex(i);
		}
		openDlg->show();
		openDlg->raise();
		openDlg->activateWindow();
		return;
	}

	QMainWindow *main = mainWindow();
	QDialog &dlg = *new QDialog(main);
	dlg.setAttribute(Qt::WA_DeleteOnClose);
	openDlg = &dlg;
	dlg.setWindowTitle("DockX");
	dlg.setMinimumSize(980, 600); /* floor; widened below to fit the tab row */

	QVBoxLayout *root = new QVBoxLayout(&dlg);
	QTabWidget *tabs = new QTabWidget(&dlg);
	root->addWidget(tabs);

	/* ---------- Find tab (project-wide source search) ---------- */
	QWidget *findTab = new QWidget();
	QVBoxLayout *findV = new QVBoxLayout(findTab);

	QLineEdit *findBox = new QLineEdit(findTab);
	findBox->setPlaceholderText("Search every source in your project by name, type, or scene");
	findBox->setClearButtonEnabled(true);
	findV->addWidget(findBox);

	QTreeWidget *findTree = new QTreeWidget(findTab);
	findTree->setColumnCount(3);
	findTree->setHeaderLabels({"Source", "Type", "In scene"});
	findTree->setRootIsDecorated(false);
	findTree->setAlternatingRowColors(true);
	findTree->setSelectionMode(QAbstractItemView::SingleSelection);
	findTree->setSortingEnabled(true);
	findTree->sortByColumn(0, Qt::AscendingOrder);
	findTree->header()->setStretchLastSection(true);
	findV->addWidget(findTree, 1);

	QLabel *findStatus = new QLabel(findTab);
	findV->addWidget(findStatus);

	/* the full scan, rebuilt on open and on Refresh; filtered in memory */
	auto scan = std::make_shared<QList<search::Hit>>();
	/* sources deleted this session: hide them even if a live ref still
	   lingers (OBS won't save a removed source, so it is gone for good) */
	auto removed = std::make_shared<QSet<QString>>();

	auto repopulate = [findTree, findStatus, scan, removed](const QString &qRaw) {
		const QString q = qRaw.trimmed();
		findTree->setSortingEnabled(false);
		findTree->clear();
		int shown = 0, unused = 0;
		for (const search::Hit &h : *scan) {
			if (removed->contains(h.sourceName))
				continue;
			const QString loc =
				h.sceneUuid.isEmpty()
					? QStringLiteral("(unused)")
					: (h.groupName.isEmpty() ? h.sceneName : h.sceneName + "  ›  " + h.groupName);
			if (!q.isEmpty() && !h.sourceName.contains(q, Qt::CaseInsensitive) &&
			    !h.sourceType.contains(q, Qt::CaseInsensitive) && !loc.contains(q, Qt::CaseInsensitive))
				continue;
			QTreeWidgetItem *it = new QTreeWidgetItem(findTree);
			it->setText(0, h.sourceName);
			it->setText(1, h.sourceType);
			it->setText(2, loc);
			it->setData(0, Qt::UserRole, h.sceneUuid);
			it->setData(0, Qt::UserRole + 1, (qlonglong)h.itemId);
			if (h.sceneUuid.isEmpty()) {
				it->setForeground(0, QBrush(QColor(150, 150, 150, 160)));
				it->setForeground(2, QBrush(QColor(150, 150, 150, 160)));
				unused++;
			} else if (!h.visible) {
				it->setForeground(0, QBrush(QColor(150, 150, 150, 140)));
			}
			shown++;
		}
		findTree->setSortingEnabled(true);
		for (int c = 0; c < 3; c++)
			findTree->resizeColumnToContents(c);
		findStatus->setText(
			QString("%1 shown · %2 unused source%3").arg(shown).arg(unused).arg(unused == 1 ? "" : "s"));
	};

	auto rescan = [scan, repopulate, findBox]() {
		*scan = search::findAll();
		repopulate(findBox->text());
	};
	rescan();

	QObject::connect(findBox, &QLineEdit::textChanged, findTab, [repopulate](const QString &t) { repopulate(t); });

	auto doProps = [findTree]() {
		QTreeWidgetItem *it = findTree->currentItem();
		if (it)
			search::openProperties(it->text(0));
	};

	/* placed source -> jump to it; orphan (no scene) -> show what it is */
	auto goToOrInspect = [findTree, doProps]() {
		QTreeWidgetItem *it = findTree->currentItem();
		if (!it)
			return;
		const QString uuid = it->data(0, Qt::UserRole).toString();
		if (uuid.isEmpty()) {
			doProps();
			return;
		}
		search::reveal(uuid, it->data(0, Qt::UserRole + 1).toLongLong());
	};

	/* drop the source from just the one scene this row represents */
	auto doRemoveFromScene = [findTree, rescan]() {
		QTreeWidgetItem *it = findTree->currentItem();
		if (!it)
			return;
		const QString uuid = it->data(0, Qt::UserRole).toString();
		if (uuid.isEmpty())
			return; /* not placed in any scene */
		if (QMessageBox::question(findTree, "DockX",
					  QString("Remove \"%1\" from the scene \"%2\"? The source "
						  "stays in your project and in any other scenes it "
						  "is in.")
						  .arg(it->text(0), it->text(2)),
					  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return;
		search::removeFromScene(uuid, it->data(0, Qt::UserRole + 1).toLongLong());
		rescan();
	};

	auto doDelete = [findTree, scan, removed, rescan]() {
		QTreeWidgetItem *it = findTree->currentItem();
		if (!it)
			return;
		const QString name = it->text(0);
		QSet<QString> sceneSet;
		for (const search::Hit &h : *scan)
			if (h.sourceName == name && !h.sceneUuid.isEmpty())
				sceneSet.insert(h.sceneUuid);
		const int scenes = sceneSet.size();
		const QString msg = scenes == 0 ? QString("\"%1\" is not used in any scene. Delete it "
							  "from your project permanently? This cannot "
							  "be undone.")
							  .arg(name)
						: QString("\"%1\" is used in %2 scene%3. Deleting it "
							  "removes it from all of them permanently. "
							  "This cannot be undone.\n\nDelete it?")
							  .arg(name)
							  .arg(scenes)
							  .arg(scenes == 1 ? "" : "s");
		if (QMessageBox::warning(findTree, "DockX", msg, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
		    QMessageBox::Yes)
			return;
		const bool freed = search::deleteSource(name);
		/* removed from the project either way (OBS won't save it), so drop it
		   from the list now; if a live ref lingers, say what holds it */
		removed->insert(name);
		if (!freed) {
			const QString who = search::describeHolders(name);
			QString info = QString("\"%1\" has been removed from your project and "
					       "will not come back after you restart OBS. ")
					       .arg(name);
			if (!who.isEmpty())
				info += "It is still open this session because " + who +
					". Fix that and it clears right away, or just "
					"restart OBS.";
			else
				info += "Something in this session is still holding it "
					"live, usually a browser dock, a Streamer.bot or "
					"websocket link, or another plugin. It clears on "
					"its own when you restart OBS.";
			QMessageBox::information(findTree, "DockX", info);
		}
		rescan();
	};

	QObject::connect(findTree, &QTreeWidget::itemDoubleClicked, findTab,
			 [goToOrInspect](QTreeWidgetItem *, int) { goToOrInspect(); });

	/* right-click a result for the full action set */
	findTree->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(findTree, &QTreeWidget::customContextMenuRequested, findTab,
			 [findTree, goToOrInspect, doProps, doRemoveFromScene, doDelete](const QPoint &pos) {
				 QTreeWidgetItem *it = findTree->itemAt(pos);
				 if (!it)
					 return;
				 findTree->setCurrentItem(it);
				 const bool placed = !it->data(0, Qt::UserRole).toString().isEmpty();
				 QMenu menu(findTree);
				 if (placed) {
					 QObject::connect(menu.addAction("Go to source"), &QAction::triggered, findTree,
							  [goToOrInspect]() { goToOrInspect(); });
				 }
				 QObject::connect(menu.addAction("Properties"), &QAction::triggered, findTree,
						  [doProps]() { doProps(); });
				 if (placed) {
					 QObject::connect(
						 menu.addAction(QString("Remove from scene \"%1\"").arg(it->text(2))),
						 &QAction::triggered, findTree,
						 [doRemoveFromScene]() { doRemoveFromScene(); });
				 }
				 menu.addSeparator();
				 QObject::connect(menu.addAction("Delete source from project"), &QAction::triggered,
						  findTree, [doDelete]() { doDelete(); });
				 menu.exec(findTree->viewport()->mapToGlobal(pos));
			 });

	QHBoxLayout *findBtns = new QHBoxLayout();
	QPushButton *revealBtn = new QPushButton("Go to source", findTab);
	QPushButton *refreshBtn = new QPushButton("Refresh", findTab);
	findBtns->addWidget(revealBtn);
	findBtns->addStretch(1);
	findBtns->addWidget(refreshBtn);
	findV->addLayout(findBtns);
	QObject::connect(revealBtn, &QPushButton::clicked, findTab, [goToOrInspect]() { goToOrInspect(); });
	QObject::connect(refreshBtn, &QPushButton::clicked, findTab, [rescan]() { rescan(); });

	/* an unused source has no scene to jump to, so the same button opens its
	   Properties instead -- SAY so, or the click reads as "nothing happened" */
	QObject::connect(findTree, &QTreeWidget::currentItemChanged, findTab,
			 [revealBtn](QTreeWidgetItem *cur, QTreeWidgetItem *) {
				 const bool unusedSel = cur && cur->data(0, Qt::UserRole).toString().isEmpty();
				 revealBtn->setText(unusedSel ? "Open properties (in no scene)" : "Go to source");
				 revealBtn->setToolTip(unusedSel ? "This source is loaded in your project but not "
								   "placed in any scene, so there is nowhere to "
								   "jump to. This opens its Properties so you can "
								   "see what it is."
								 : QString());
			 });

	QLabel *findHint = new QLabel("Search every scene in this collection at once. Double-click a "
				      "result to jump to that scene and select it. Right-click any "
				      "result for more: open its Properties, remove it from just that "
				      "scene, or delete it from the whole project. Sources listed as "
				      "(unused) are loaded in your project but not placed in any scene "
				      "(OBS can't show these), so right-click to identify or clear them.",
				      findTab);
	findHint->setWordWrap(true);
	findV->addWidget(findHint);

	tabs->addTab(findTab, "Find");

	/* ---------- Layouts tab ---------- */
	QWidget *layoutsTab = new QWidget();
	QHBoxLayout *lcols = new QHBoxLayout(layoutsTab);
	QVBoxLayout *lv = new QVBoxLayout();  /* left: dock layouts + templates */
	QVBoxLayout *lvR = new QVBoxLayout(); /* right: source loadouts + auto switch */
	lcols->addLayout(lv, 1);
	lcols->addLayout(lvR, 1);

	/* everything arrangement shaped lives on this one tab: dock layouts,
	   starter templates, source loadouts, scene auto switch. Joey: separate
	   tabs were noise, and layouts vs loadouts MUST be spelled out (he
	   found the split confusing and he built it) */
	QGroupBox *savedBox = new QGroupBox("Dock layouts (your panels)", layoutsTab);
	QVBoxLayout *slv = new QVBoxLayout(savedBox);
	slv->addWidget(groupSub("Where your panels sit around the screen. Save one per way you stream.", savedBox));

	QListWidget *layoutList = new HintList("No layouts saved yet.\n\nArrange your docks the way you like, "
					       "then click Save current layout.",
					       savedBox);
	slv->addWidget(layoutList, 1);

	auto reloadLayouts = [layoutList]() {
		layoutList->clear();
		for (const Layout &l : state().layouts) {
			QString text = l.name;
			const QString hk = layoutHotkeyText(l);
			if (!hk.isEmpty())
				text += QString("   [%1]").arg(hk);
			QListWidgetItem *it = new QListWidgetItem(text, layoutList);
			it->setData(Qt::UserRole, l.id);
		}
	};
	auto selectedLayoutId = [layoutList]() -> int {
		QListWidgetItem *it = layoutList->currentItem();
		return it ? it->data(Qt::UserRole).toInt() : 0;
	};
	reloadLayouts();

	QHBoxLayout *lb = new QHBoxLayout();
	QPushButton *saveBtn = new QPushButton("Save current layout", layoutsTab);
	makePrimary(saveBtn);
	saveBtn->setToolTip("Snapshots which docks are open and where they sit right now.");
	QPushButton *applyBtn = new QPushButton("Apply", layoutsTab);
	applyBtn->setToolTip("Rearrange your docks to the selected layout. You can always undo. "
			     "Double clicking a layout applies it too.");
	applyBtn->setEnabled(false);
	QPushButton *undoBtn = new QPushButton("Undo apply", layoutsTab);
	undoBtn->setToolTip("Put the docks back the way they were before the last apply.");
	QPushButton *moreBtn = new QPushButton("More", layoutsTab);
	lb->addWidget(saveBtn);
	lb->addWidget(applyBtn);
	lb->addWidget(undoBtn);
	lb->addWidget(moreBtn);
	lb->addStretch(1);
	slv->addLayout(lb);

	/* rename/delete/hotkeys live behind More and behind a right click on the
	   list: available, not shouting */
	QMenu *layoutMenu = new QMenu(moreBtn);
	layoutMenu->setToolTipsVisible(true);
	QAction *actHotkey = layoutMenu->addAction("Set hotkey...");
	actHotkey->setToolTip("Bind a key that applies the selected layout; a Stream Deck can "
			      "press that key for one tap changes.");
	QAction *actUnbind = layoutMenu->addAction("Remove hotkey");
	layoutMenu->addSeparator();
	QAction *actRename = layoutMenu->addAction("Rename...");
	QAction *actDelete = layoutMenu->addAction("Delete");
	moreBtn->setMenu(layoutMenu);
	layoutList->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(layoutList, &QListWidget::customContextMenuRequested, layoutList,
			 [layoutList, layoutMenu](const QPoint &pos) {
				 if (QListWidgetItem *it = layoutList->itemAt(pos))
					 layoutList->setCurrentItem(it);
				 layoutMenu->exec(layoutList->viewport()->mapToGlobal(pos));
			 });
	QObject::connect(layoutList, &QListWidget::itemSelectionChanged, applyBtn,
			 [applyBtn, layoutList]() { applyBtn->setEnabled(layoutList->currentItem() != nullptr); });
	QObject::connect(layoutList, &QListWidget::itemDoubleClicked, &dlg,
			 [](QListWidgetItem *it) { panels::applyLayout(it->data(Qt::UserRole).toInt()); });

	QObject::connect(saveBtn, &QPushButton::clicked, &dlg, [&dlg, reloadLayouts]() {
		QMainWindow *m = mainWindow();
		if (!m)
			return;
		bool ok = false;
		QString suggested = QString("Layout %1").arg((int)state().layouts.size() + 1);
		QString name = QInputDialog::getText(&dlg, "Save layout", "Name this dock layout:", QLineEdit::Normal,
						     suggested, &ok);
		name = name.trimmed();
		if (!ok || name.isEmpty())
			return;
		addLayout(name, m->saveState());
		reloadLayouts();
	});
	QObject::connect(applyBtn, &QPushButton::clicked, &dlg, [&dlg, selectedLayoutId]() {
		int id = selectedLayoutId();
		if (!id) {
			QMessageBox::information(&dlg, "DockX", "Pick a layout first.");
			return;
		}
		panels::applyLayout(id);
	});
	QObject::connect(actRename, &QAction::triggered, &dlg, [&dlg, selectedLayoutId, reloadLayouts]() {
		int id = selectedLayoutId();
		Layout *l = id ? findLayout(id) : nullptr;
		if (!l)
			return;
		bool ok = false;
		QString name =
			QInputDialog::getText(&dlg, "Rename layout", "New name:", QLineEdit::Normal, l->name, &ok);
		name = name.trimmed();
		if (!ok || name.isEmpty())
			return;
		renameLayout(id, name);
		reloadLayouts();
	});
	QObject::connect(actDelete, &QAction::triggered, &dlg, [&dlg, selectedLayoutId, reloadLayouts]() {
		int id = selectedLayoutId();
		Layout *l = id ? findLayout(id) : nullptr;
		if (!l)
			return;
		auto answer = QMessageBox::question(&dlg, "Delete layout",
						    QString("Delete \"%1\"? Your current dock arrangement "
							    "is not touched.")
							    .arg(l->name));
		if (answer != QMessageBox::Yes)
			return;
		removeLayout(id);
		reloadLayouts();
	});
	QObject::connect(undoBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		if (!panels::undoLayout())
			QMessageBox::information(&dlg, "DockX", "Nothing to undo yet.");
	});
	QObject::connect(actHotkey, &QAction::triggered, &dlg, [&dlg, selectedLayoutId, reloadLayouts]() {
		int id = selectedLayoutId();
		Layout *l = id ? findLayout(id) : nullptr;
		if (!l) {
			QMessageBox::information(&dlg, "DockX", "Pick a layout first.");
			return;
		}
		QKeySequence seq;
		const int r = promptHotkey(&dlg, QString("Hotkey for \"%1\"").arg(l->name), seq);
		if (r == 0)
			return;
		if (r == 1 && !setLayoutHotkey(l, seq))
			QMessageBox::information(&dlg, "DockX",
						 "That key is not supported here. You can still "
						 "bind it in OBS under Settings > Hotkeys "
						 "(search for DockX).");
		if (r == 2)
			setLayoutHotkey(l, QKeySequence());
		reloadLayouts();
	});
	QObject::connect(actUnbind, &QAction::triggered, &dlg, [&dlg, selectedLayoutId, reloadLayouts]() {
		int id = selectedLayoutId();
		Layout *l = id ? findLayout(id) : nullptr;
		if (!l) {
			QMessageBox::information(&dlg, "DockX", "Pick a layout first.");
			return;
		}
		setLayoutHotkey(l, QKeySequence());
		reloadLayouts();
	});

	lv->addWidget(savedBox, 3);

	/* ---------- starter templates (one-click starting points) ---------- */
	QGroupBox *tplTab = new QGroupBox("Starter templates", layoutsTab);
	QVBoxLayout *tplV = new QVBoxLayout(tplTab);

	QListWidget *tplList = new QListWidget(tplTab);
	for (const templates::Info &t : templates::list()) {
		QListWidgetItem *it = new QListWidgetItem(t.name, tplList);
		it->setData(Qt::UserRole, t.id);
		it->setData(Qt::UserRole + 1, t.desc);
	}
	tplList->setMaximumHeight(96);
	tplV->addWidget(tplList);

	QLabel *tplDesc = groupSub("Ready made starting points. Pick one to see what it does; "
				   "applying always keeps an undo.",
				   tplTab);
	tplV->addWidget(tplDesc);

	auto tplSelId = [tplList]() -> QString {
		QListWidgetItem *it = tplList->currentItem();
		return it ? it->data(Qt::UserRole).toString() : QString();
	};
	QObject::connect(tplList, &QListWidget::currentItemChanged, tplTab,
			 [tplDesc](QListWidgetItem *cur, QListWidgetItem *) {
				 if (cur)
					 tplDesc->setText(cur->data(Qt::UserRole + 1).toString());
			 });

	QHBoxLayout *tplBtns = new QHBoxLayout();
	QPushButton *tplApply = new QPushButton("Apply template", tplTab);
	tplApply->setEnabled(false);
	tplApply->setToolTip("Hides OBS's fixed preview, adds a DockX Preview, and arranges "
			     "your docks. Safe to try: Undo apply puts everything back.");
	QPushButton *tplUndo = new QPushButton("Undo apply", tplTab);
	tplBtns->addWidget(tplApply);
	tplBtns->addWidget(tplUndo);
	tplBtns->addStretch(1);
	tplV->addLayout(tplBtns);

	QObject::connect(tplList, &QListWidget::itemSelectionChanged, tplApply,
			 [tplApply, tplList]() { tplApply->setEnabled(tplList->currentItem() != nullptr); });
	auto applyTemplate = [&dlg, tplSelId]() {
		const QString id = tplSelId();
		if (id.isEmpty())
			return;
		if (!templates::apply(id, &dlg))
			QMessageBox::information(&dlg, "DockX",
						 "Could not apply that template. Your layout was not changed.");
	};
	QObject::connect(tplApply, &QPushButton::clicked, &dlg, applyTemplate);
	QObject::connect(tplList, &QListWidget::itemDoubleClicked, &dlg,
			 [applyTemplate](QListWidgetItem *) { applyTemplate(); });
	QObject::connect(tplUndo, &QPushButton::clicked, &dlg, [&dlg]() {
		if (!panels::undoLayout())
			QMessageBox::information(&dlg, "DockX", "Nothing to undo yet.");
	});

	lv->addWidget(tplTab, 1);

	tabs->addTab(layoutsTab, "Layouts");

	/* ---------- source loadouts (source positions, LoadoutX ported) ---------- */
	QGroupBox *loTab = new QGroupBox("Source loadouts (inside your scenes)", layoutsTab);
	QVBoxLayout *lov = new QVBoxLayout(loTab);
	lov->addWidget(groupSub("Where your sources sit inside a scene: position, size, crop, "
				"visibility. Layouts are your panels; loadouts are your sources.",
				loTab));

	QListWidget *loList = new HintList("No loadouts saved yet.\n\nSet a scene up perfectly, then click "
					   "Save current scene. If things get nudged mid stream, Restore "
					   "snaps them all back.",
					   loTab);
	lov->addWidget(loList, 1);

	auto reloadLoadouts = [loList]() {
		loList->clear();
		for (const SourceLoadout &l : state().loadouts) {
			const QString scope = l.sceneUuid.isEmpty() ? "All scenes" : l.sceneName;
			QListWidgetItem *it = new QListWidgetItem(
				QString("%1   ·   %2   ·   %3 sources").arg(l.name, scope).arg((int)l.items.size()),
				loList);
			it->setData(Qt::UserRole, l.id);
		}
	};
	auto selectedLoadout = [loList]() -> SourceLoadout * {
		QListWidgetItem *it = loList->currentItem();
		if (!it)
			return nullptr;
		const int id = it->data(Qt::UserRole).toInt();
		for (SourceLoadout &l : state().loadouts)
			if (l.id == id)
				return &l;
		return nullptr;
	};
	reloadLoadouts();

	auto saveLoadout = [&dlg, reloadLoadouts](bool allScenes) {
		QString uuid, sceneName;
		if (!allScenes) {
			obs_source_t *cur = obs_frontend_get_current_scene();
			if (!cur) {
				QMessageBox::information(&dlg, "DockX", "No current scene to save.");
				return;
			}
			uuid = QString::fromUtf8(obs_source_get_uuid(cur));
			sceneName = QString::fromUtf8(obs_source_get_name(cur));
			obs_source_release(cur);
		}
		const QString suggested = allScenes ? QString("Everything %1").arg((int)state().loadouts.size() + 1)
						    : QString("%1 loadout").arg(sceneName);
		bool ok = false;
		QString name = QInputDialog::getText(&dlg, "Save loadout", "Name this loadout:", QLineEdit::Normal,
						     suggested, &ok)
				       .trimmed();
		if (!ok || name.isEmpty())
			return;
		SourceLoadout l = loadouts::capture(uuid, sceneName);
		if (l.items.empty()) {
			QMessageBox::information(&dlg, "DockX", "There are no sources to save yet.");
			return;
		}
		l.id = state().nextLoadoutId++;
		l.name = name;
		state().loadouts.push_back(l);
		stateSave();
		reloadLoadouts();
	};

	QHBoxLayout *lob = new QHBoxLayout();
	QPushButton *loSaveCur = new QPushButton("Save current scene", loTab);
	makePrimary(loSaveCur);
	loSaveCur->setToolTip("Saves where every source in the current scene sits: position, size, "
			      "rotation, crop, visibility, lock.");
	QPushButton *loSaveAll = new QPushButton("Save all scenes", loTab);
	loSaveAll->setToolTip("One loadout that covers every scene at once.");
	QPushButton *loRestore = new QPushButton("Restore", loTab);
	loRestore->setToolTip("Snap every saved source back to its saved spot. You can always undo. "
			      "Double clicking a loadout restores it too.");
	loRestore->setEnabled(false);
	QPushButton *loUndo = new QPushButton("Undo restore", loTab);
	loUndo->setToolTip("Put things back the way they were before the restore. Press it twice "
			   "to flip forward again.");
	QPushButton *loMore = new QPushButton("More", loTab);
	lob->addWidget(loSaveCur);
	lob->addWidget(loSaveAll);
	lob->addWidget(loRestore);
	lob->addWidget(loUndo);
	lob->addWidget(loMore);
	lob->addStretch(1);
	lov->addLayout(lob);

	QMenu *loMenu = new QMenu(loMore);
	loMenu->setToolTipsVisible(true);
	QAction *loActRename = loMenu->addAction("Rename...");
	QAction *loActDelete = loMenu->addAction("Delete");
	loMenu->addSeparator();
	QAction *loActExport = loMenu->addAction("Back up to file...");
	loActExport->setToolTip("Saves your loadouts to a file, to move them to another PC or share them.");
	QAction *loActImport = loMenu->addAction("Import from file...");
	loActImport->setToolTip("Adds loadouts from a backup file; nothing of yours is overwritten.");
	loMore->setMenu(loMenu);
	loList->setContextMenuPolicy(Qt::CustomContextMenu);
	QObject::connect(loList, &QListWidget::customContextMenuRequested, loList, [loList, loMenu](const QPoint &pos) {
		if (QListWidgetItem *it = loList->itemAt(pos))
			loList->setCurrentItem(it);
		loMenu->exec(loList->viewport()->mapToGlobal(pos));
	});
	QObject::connect(loList, &QListWidget::itemSelectionChanged, loRestore,
			 [loRestore, loList]() { loRestore->setEnabled(loList->currentItem() != nullptr); });

	QObject::connect(loSaveCur, &QPushButton::clicked, &dlg, [saveLoadout]() { saveLoadout(false); });
	QObject::connect(loSaveAll, &QPushButton::clicked, &dlg, [saveLoadout]() { saveLoadout(true); });
	auto restoreSelected = [&dlg, selectedLoadout]() {
		SourceLoadout *l = selectedLoadout();
		if (!l)
			return;
		const QString scope = l->sceneUuid.isEmpty() ? "every scene" : l->sceneName;
		const auto answer =
			QMessageBox::question(&dlg, "Restore loadout",
					      QString("Every saved source in %1 snaps back to its saved spot. "
						      "You can undo this. Restore \"%2\"?")
						      .arg(scope, l->name));
		if (answer != QMessageBox::Yes)
			return;
		const loadouts::RestoreReport r = loadouts::restore(*l);
		folders::rebuildSoon();
		QString msg = QString("Restored %1 sources.").arg(r.restored);
		if (!r.missing.isEmpty()) {
			QStringList shown = r.missing.mid(0, 8);
			if (r.missing.size() > 8)
				shown << QString("and %1 more").arg(r.missing.size() - 8);
			msg += "\n\nNot found anymore:\n" + shown.join("\n");
		}
		QMessageBox::information(&dlg, "DockX", msg);
	};
	QObject::connect(loRestore, &QPushButton::clicked, &dlg, restoreSelected);
	QObject::connect(loList, &QListWidget::itemDoubleClicked, &dlg,
			 [restoreSelected](QListWidgetItem *) { restoreSelected(); });
	QObject::connect(loUndo, &QPushButton::clicked, &dlg, [&dlg]() {
		loadouts::RestoreReport r;
		if (!loadouts::undoRestore(r)) {
			QMessageBox::information(&dlg, "DockX", "Nothing to undo yet.");
			return;
		}
		folders::rebuildSoon();
		QMessageBox::information(&dlg, "DockX",
					 QString("Put %1 sources back the way they were.").arg(r.restored));
	});
	QObject::connect(loActRename, &QAction::triggered, &dlg, [&dlg, selectedLoadout, reloadLoadouts]() {
		SourceLoadout *l = selectedLoadout();
		if (!l)
			return;
		bool ok = false;
		const QString name =
			QInputDialog::getText(&dlg, "Rename loadout", "New name:", QLineEdit::Normal, l->name, &ok)
				.trimmed();
		if (!ok || name.isEmpty())
			return;
		l->name = name;
		stateSave();
		reloadLoadouts();
	});
	QObject::connect(loActDelete, &QAction::triggered, &dlg, [&dlg, selectedLoadout, reloadLoadouts]() {
		SourceLoadout *l = selectedLoadout();
		if (!l)
			return;
		const auto answer = QMessageBox::question(&dlg, "Delete loadout",
							  QString("Delete \"%1\"? Your sources are not "
								  "touched.")
								  .arg(l->name));
		if (answer != QMessageBox::Yes)
			return;
		const int id = l->id;
		auto &v = state().loadouts;
		for (size_t i = 0; i < v.size(); i++) {
			if (v[i].id == id) {
				v.erase(v.begin() + i);
				break;
			}
		}
		stateSave();
		reloadLoadouts();
	});
	QObject::connect(loActExport, &QAction::triggered, &dlg, [&dlg]() {
		if (state().loadouts.empty()) {
			QMessageBox::information(&dlg, "DockX", "You have no loadouts to back up yet.");
			return;
		}
		const QString path = QFileDialog::getSaveFileName(&dlg, "Back up loadouts", "dockx-loadouts.json",
								  "DockX loadouts (*.json)");
		if (path.isEmpty())
			return;
		if (loadouts::exportFile(path))
			QMessageBox::information(&dlg, "DockX",
						 QString("Backed up %1 loadout(s). Keep this file to move "
							 "them to another PC or share them.")
							 .arg((int)state().loadouts.size()));
		else
			QMessageBox::warning(&dlg, "DockX", "Could not write that file.");
	});
	QObject::connect(loActImport, &QAction::triggered, &dlg, [&dlg, reloadLoadouts]() {
		const QString path =
			QFileDialog::getOpenFileName(&dlg, "Import loadouts", QString(), "DockX loadouts (*.json)");
		if (path.isEmpty())
			return;
		const int n = loadouts::importFile(path);
		if (n < 0) {
			QMessageBox::warning(&dlg, "DockX", "That file could not be read as a DockX loadouts backup.");
			return;
		}
		reloadLoadouts();
		QMessageBox::information(&dlg, "DockX",
					 QString("Imported %1 loadout(s). They were added to your list; "
						 "nothing was overwritten.")
						 .arg(n));
	});

	lvR->addWidget(loTab, 2);

	/* ---------- lock groups (folded into the Layouts tab, Joey 09-10:
	   dock locking joins the panels column, source locking joins the
	   inside-your-scenes column; the separate Locks tab was noise) ---------- */

	/* -- dock layout locking -- */
	QGroupBox *dockGroup = new QGroupBox("Lock the layout", layoutsTab);
	QVBoxLayout *dg = new QVBoxLayout(dockGroup);
	dg->addWidget(groupSub("Every lock and revert here has a hotkey: OBS Settings > Hotkeys, search "
			       "DockX. Nudge your setup mid stream, hit one key (or a Stream Deck "
			       "button), and it snaps back.",
			       dockGroup));

	QCheckBox *hardLockChk =
		new QCheckBox("Lock docks in place (they can't be dragged or floated by accident)", dockGroup);
	hardLockChk->setChecked(locks::hardLock());
	hardLockChk->setToolTip("Docks stay put until you untick this. There is a hotkey for it in OBS "
				"Settings > Hotkeys (search DockX).");
	dg->addWidget(hardLockChk);
	QObject::connect(hardLockChk, &QCheckBox::toggled, dockGroup, [](bool on) { locks::setHardLock(on); });

	QLabel *pointLbl = new QLabel(dockGroup);
	pointLbl->setWordWrap(true);
	auto refreshPoint = [pointLbl]() {
		pointLbl->setText(locks::hasLockPoint() ? "Revert point saved. If a dock drifts, snap the whole "
							  "layout back with Revert to point."
							: "No revert point saved yet. Arrange your docks, then Set "
							  "revert point to lock in that spot.");
	};
	refreshPoint();
	dg->addWidget(pointLbl);

	QHBoxLayout *dgb = new QHBoxLayout();
	QPushButton *setPointBtn = new QPushButton("Set revert point", dockGroup);
	QPushButton *revertBtn = new QPushButton("Revert to point", dockGroup);
	dgb->addWidget(setPointBtn);
	dgb->addWidget(revertBtn);
	dgb->addStretch(1);
	dg->addLayout(dgb);
	QObject::connect(setPointBtn, &QPushButton::clicked, dockGroup, [refreshPoint]() {
		locks::setLockPoint();
		refreshPoint();
	});
	QObject::connect(revertBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		if (!locks::revertToLockPoint())
			QMessageBox::information(&dlg, "DockX",
						 "Set a revert point first, then this snaps your docks "
						 "back to it.");
	});
	setPointBtn->setToolTip("Saves the current dock arrangement as your snap back point.");
	revertBtn->setToolTip("Snaps every dock back to the saved point. Hotkey: OBS Settings > Hotkeys "
			      "(search DockX). Tools > DockX: Revert dock layout works too.");
	lv->addWidget(dockGroup);

	/* -- scene source locking -- */
	QGroupBox *srcGroup = new QGroupBox("Lock sources in scenes", layoutsTab);
	QVBoxLayout *sg = new QVBoxLayout(srcGroup);
	sg->addWidget(groupSub("Lock every source in a scene at once so nothing on the canvas gets "
			       "nudged by accident.",
			       srcGroup));

	auto currentSceneName = []() -> QString {
		obs_source_t *cur = obs_frontend_get_current_scene();
		QString n = cur ? QString::fromUtf8(obs_source_get_name(cur)) : QString();
		if (cur)
			obs_source_release(cur);
		return n;
	};

	QHBoxLayout *sgb = new QHBoxLayout();
	QPushButton *lockCur = new QPushButton("Lock this scene", srcGroup);
	QPushButton *unlockCur = new QPushButton("Unlock this scene", srcGroup);
	lockCur->setToolTip("Flips the same lock you see on each source, just all together.");
	unlockCur->setToolTip("Unlocks every source in the current scene.");
	sgb->addWidget(lockCur);
	sgb->addWidget(unlockCur);
	sgb->addStretch(1);
	sg->addLayout(sgb);

	QHBoxLayout *sgb2 = new QHBoxLayout();
	QPushButton *lockAllBtn = new QPushButton("Lock every scene", srcGroup);
	QPushButton *unlockAllBtn = new QPushButton("Unlock every scene", srcGroup);
	QPushButton *pickBtn = new QPushButton("Selected scenes...", srcGroup);
	sgb2->addWidget(lockAllBtn);
	sgb2->addWidget(unlockAllBtn);
	sgb2->addWidget(pickBtn);
	sgb2->addStretch(1);
	sg->addLayout(sgb2);
	/* added to the right column AFTER the auto switch box below, so the
	   column reads loadouts -> auto switch -> locks */

	QObject::connect(lockCur, &QPushButton::clicked, &dlg, [&dlg, currentSceneName]() {
		const QString n = currentSceneName();
		locks::lockCurrentScene(true);
		QMessageBox::information(&dlg, "DockX",
					 n.isEmpty() ? "Locked every source in the current scene."
						     : QString("Locked every source in \"%1\".").arg(n));
	});
	QObject::connect(unlockCur, &QPushButton::clicked, &dlg, [&dlg, currentSceneName]() {
		const QString n = currentSceneName();
		locks::lockCurrentScene(false);
		QMessageBox::information(&dlg, "DockX",
					 n.isEmpty() ? "Unlocked every source in the current scene."
						     : QString("Unlocked every source in \"%1\".").arg(n));
	});
	QObject::connect(lockAllBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		locks::lockAllScenes(true);
		QMessageBox::information(&dlg, "DockX", "Locked every source in every scene.");
	});
	QObject::connect(unlockAllBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		locks::lockAllScenes(false);
		QMessageBox::information(&dlg, "DockX", "Unlocked every source in every scene.");
	});
	QObject::connect(pickBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		QDialog pick(&dlg);
		pick.setWindowTitle("Lock selected scenes");
		pick.setMinimumWidth(340);
		QVBoxLayout *pv = new QVBoxLayout(&pick);
		pv->addWidget(new QLabel("Check the scenes, then lock or unlock them:", &pick));
		QListWidget *plist = new QListWidget(&pick);
		for (const SceneRef &r : sceneRefs()) {
			QListWidgetItem *it = new QListWidgetItem(r.name, plist);
			it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
			it->setCheckState(Qt::Unchecked);
			it->setData(Qt::UserRole, r.uuid);
		}
		pv->addWidget(plist, 1);
		QHBoxLayout *pb = new QHBoxLayout();
		QPushButton *lockSel = new QPushButton("Lock checked", &pick);
		QPushButton *unlockSel = new QPushButton("Unlock checked", &pick);
		QPushButton *cancelSel = new QPushButton("Cancel", &pick);
		pb->addStretch(1);
		pb->addWidget(lockSel);
		pb->addWidget(unlockSel);
		pb->addWidget(cancelSel);
		pv->addLayout(pb);
		int action = 0; /* 1 = lock, 2 = unlock */
		QObject::connect(lockSel, &QPushButton::clicked, &pick, [&]() {
			action = 1;
			pick.accept();
		});
		QObject::connect(unlockSel, &QPushButton::clicked, &pick, [&]() {
			action = 2;
			pick.accept();
		});
		QObject::connect(cancelSel, &QPushButton::clicked, &pick, [&pick]() { pick.reject(); });
		if (pick.exec() != QDialog::Accepted || action == 0)
			return;
		QStringList uuids;
		for (int i = 0; i < plist->count(); i++) {
			QListWidgetItem *it = plist->item(i);
			if (it->checkState() == Qt::Checked)
				uuids << it->data(Qt::UserRole).toString();
		}
		if (uuids.isEmpty())
			return;
		locks::lockScenes(uuids, action == 1);
		QMessageBox::information(&dlg, "DockX",
					 QString("%1 every source in %2 scene(s).")
						 .arg(action == 1 ? "Locked" : "Unlocked")
						 .arg(uuids.size()));
	});

	/* ---------- Auto switch tab ---------- */
	QGroupBox *autoTab = new QGroupBox("Auto switch dock layouts by scene", layoutsTab);
	QVBoxLayout *av = new QVBoxLayout(autoTab);

	av->addWidget(groupSub("Link an OBS scene to a dock layout. Want your panels arranged differently for "
			       "certain scenes? Pair them up and OBS rearranges itself every time the scene "
			       "changes.",
			       autoTab));

	QListWidget *ruleList = new HintList("Nothing paired yet.\n\nClick Pair scene with layout to have a "
					     "scene bring its own dock arrangement: a gameplay layout, a "
					     "chatting layout, an ending layout.",
					     autoTab);
	av->addWidget(ruleList, 1);

	auto reloadRules = [ruleList]() {
		ruleList->clear();
		QStringList scenes = state().sceneLayouts.keys();
		scenes.sort(Qt::CaseInsensitive);
		for (const QString &scene : scenes) {
			Layout *l = findLayout(state().sceneLayouts.value(scene));
			if (!l)
				continue;
			QListWidgetItem *it =
				new QListWidgetItem(QString("%1   applies   %2").arg(scene, l->name), ruleList);
			it->setData(Qt::UserRole, scene);
		}
	};
	reloadRules();

	QHBoxLayout *ab = new QHBoxLayout();
	QPushButton *addRuleBtn = new QPushButton("Pair scene with layout", autoTab);
	makePrimary(addRuleBtn);
	addRuleBtn->setToolTip("Tip: a Stream Deck button that switches the scene will pull the "
			       "matching layout with it automatically.");
	QPushButton *removeRuleBtn = new QPushButton("Remove pairing", autoTab);
	removeRuleBtn->setEnabled(false);
	ab->addWidget(addRuleBtn);
	ab->addWidget(removeRuleBtn);
	ab->addStretch(1);
	av->addLayout(ab);

	QObject::connect(ruleList, &QListWidget::itemSelectionChanged, removeRuleBtn, [removeRuleBtn, ruleList]() {
		removeRuleBtn->setEnabled(ruleList->currentItem() != nullptr);
	});

	QObject::connect(addRuleBtn, &QPushButton::clicked, &dlg, [&dlg, reloadRules]() {
		if (state().layouts.empty()) {
			QMessageBox::information(&dlg, "DockX", "Save a layout first (Layouts tab).");
			return;
		}
		QDialog rd(&dlg);
		rd.setWindowTitle("Pair scene with layout");
		QVBoxLayout *v = new QVBoxLayout(&rd);
		v->addWidget(new QLabel("When OBS switches to this scene:", &rd));
		QComboBox *sceneBox = new QComboBox(&rd);
		sceneBox->addItems(sceneNames());
		v->addWidget(sceneBox);
		v->addWidget(new QLabel("rearrange my docks to this layout:", &rd));
		QComboBox *layoutBox = new QComboBox(&rd);
		for (const Layout &l : state().layouts)
			layoutBox->addItem(l.name, l.id);
		v->addWidget(layoutBox);
		QHBoxLayout *hb = new QHBoxLayout();
		QPushButton *ok = new QPushButton("Pair them", &rd);
		QPushButton *cancel = new QPushButton("Cancel", &rd);
		hb->addStretch(1);
		hb->addWidget(ok);
		hb->addWidget(cancel);
		v->addLayout(hb);
		QObject::connect(ok, &QPushButton::clicked, &rd, [&rd]() { rd.accept(); });
		QObject::connect(cancel, &QPushButton::clicked, &rd, [&rd]() { rd.reject(); });
		if (rd.exec() != QDialog::Accepted || sceneBox->currentText().isEmpty())
			return;
		state().sceneLayouts[sceneBox->currentText()] = layoutBox->currentData().toInt();
		stateSave();
		reloadRules();
	});
	QObject::connect(removeRuleBtn, &QPushButton::clicked, &dlg, [&dlg, ruleList, reloadRules]() {
		QListWidgetItem *it = ruleList->currentItem();
		if (!it) {
			QMessageBox::information(&dlg, "DockX", "Pick a pairing first.");
			return;
		}
		state().sceneLayouts.remove(it->data(Qt::UserRole).toString());
		stateSave();
		reloadRules();
	});

	lvR->addWidget(autoTab, 1);
	lvR->addWidget(srcGroup);

	/* ---------- Filters tab ---------- */
	QWidget *filtersTab = new QWidget();
	QVBoxLayout *fv = new QVBoxLayout(filtersTab);

	QListWidget *filterListW = new QListWidget(filtersTab);
	fv->addWidget(filterListW, 1);

	auto reloadFilters = [filterListW]() {
		filterListW->clear();
		for (const filters::Entry &e : filters::entries()) {
			QString text = QString("%1 · %2").arg(e.sourceName, e.filterName);
			const QString hk = hotkeyIdText(e.hotkey);
			if (!hk.isEmpty())
				text += QString("   [%1]").arg(hk);
			QListWidgetItem *it = new QListWidgetItem(text, filterListW);
			it->setData(Qt::UserRole, (qulonglong)e.hotkey);
		}
	};
	reloadFilters();

	QHBoxLayout *fb = new QHBoxLayout();
	QPushButton *filterKeyBtn = new QPushButton("Set hotkey", filtersTab);
	QPushButton *filterUnbindBtn = new QPushButton("Remove hotkey", filtersTab);
	fb->addWidget(filterKeyBtn);
	fb->addWidget(filterUnbindBtn);
	fb->addStretch(1);
	fv->addLayout(fb);

	QObject::connect(filterUnbindBtn, &QPushButton::clicked, &dlg, [&dlg, filterListW, reloadFilters]() {
		QListWidgetItem *it = filterListW->currentItem();
		if (!it) {
			QMessageBox::information(&dlg, "DockX", "Pick a filter first.");
			return;
		}
		applyHotkeyBinding((obs_hotkey_id)it->data(Qt::UserRole).toULongLong(), QKeySequence());
		reloadFilters();
	});

	QObject::connect(filterKeyBtn, &QPushButton::clicked, &dlg, [&dlg, filterListW, reloadFilters]() {
		QListWidgetItem *it = filterListW->currentItem();
		if (!it) {
			QMessageBox::information(&dlg, "DockX", "Pick a filter first.");
			return;
		}
		const obs_hotkey_id id = (obs_hotkey_id)it->data(Qt::UserRole).toULongLong();
		QKeySequence seq;
		const int r = promptHotkey(&dlg, "Filter hotkey", seq);
		if (r == 0)
			return;
		if (r == 1 && !applyHotkeyBinding(id, seq))
			QMessageBox::information(&dlg, "DockX",
						 "That key is not supported here. You can still "
						 "bind it in OBS under Settings > Hotkeys.");
		if (r == 2)
			applyHotkeyBinding(id, QKeySequence());
		reloadFilters();
	});

	QLabel *fhint = new QLabel("Every filter on every source gets its own on/off hotkey, saved with your "
				   "scene collection. Bind keys here, and press them live (or from a Stream "
				   "Deck) to toggle the filter.",
				   filtersTab);
	fhint->setWordWrap(true);
	fv->addWidget(fhint);

	tabs->addTab(filtersTab, "Filters");

	/* ---------- Colors tab (scene names + docks + one click looks) ---------- */
	QWidget *colorsTab = new QWidget();
	QVBoxLayout *cvRoot = new QVBoxLayout(colorsTab);
	QHBoxLayout *ccols = new QHBoxLayout();
	cvRoot->addLayout(ccols, 1);

	QGroupBox *sceneColBox = new QGroupBox("Scene names", colorsTab);
	QVBoxLayout *cv = new QVBoxLayout(sceneColBox);

	QListWidget *sceneListW = new QListWidget(sceneColBox);
	sceneListW->setSelectionMode(QAbstractItemView::ExtendedSelection);
	for (const QString &name : sceneNames()) {
		QListWidgetItem *it = new QListWidgetItem(name, sceneListW);
		const QString hex = state().colors.value(name);
		if (!hex.isEmpty()) {
			it->setForeground(QBrush(QColor(hex)));
			it->setIcon(colorDot(QColor(hex)));
		}
	}
	cv->addWidget(sceneListW, 1);

	auto setSceneColor = [sceneListW, &dlg](const QString &hex) {
		const QList<QListWidgetItem *> sel = sceneListW->selectedItems();
		if (sel.isEmpty()) {
			QMessageBox::information(&dlg, "DockX", "Pick one or more scenes first.");
			return;
		}
		for (QListWidgetItem *it : sel) {
			if (hex.isEmpty()) {
				state().colors.remove(it->text());
				it->setData(Qt::ForegroundRole, QVariant());
				it->setIcon(QIcon());
			} else {
				state().colors[it->text()] = hex;
				it->setForeground(QBrush(QColor(hex)));
				it->setIcon(colorDot(QColor(hex)));
			}
		}
		stateSave();
		panels::refreshSoon();
	};

	addPaletteRow(sceneColBox, cv, &dlg, setSceneColor);

	QLabel *chint = new QLabel("Pick a scene (Ctrl click or Shift click for several at "
				   "once), then a color. The scene name shows in that color in "
				   "the Scenes panel. Sources already have this built into OBS: "
				   "right click a source and pick Set Color.",
				   sceneColBox);
	chint->setWordWrap(true);
	cv->addWidget(chint);

	ccols->addWidget(sceneColBox, 1);

	/* ---------- dock colors (border/title + background + title fade) ---------- */
	QGroupBox *dockTab = new QGroupBox("Docks", colorsTab);
	QVBoxLayout *dv = new QVBoxLayout(dockTab);

	QListWidget *dockListW = new QListWidget(dockTab);
	dockListW->setSelectionMode(QAbstractItemView::ExtendedSelection);
	auto reloadDocks = [dockListW]() {
		dockListW->clear();
		for (const panels::DockInfo &info : panels::listDocks()) {
			QListWidgetItem *it = new QListWidgetItem(info.title, dockListW);
			it->setData(Qt::UserRole, info.key);
			const QString hex = state().dockColorMap.value(info.key);
			if (!hex.isEmpty()) {
				it->setForeground(QBrush(QColor(hex)));
				it->setIcon(colorDot(QColor(hex)));
			}
		}
	};
	reloadDocks();
	dv->addWidget(dockListW, 1);

	auto setDockColor = [dockListW, &dlg](const QString &hex) {
		const QList<QListWidgetItem *> sel = dockListW->selectedItems();
		if (sel.isEmpty()) {
			QMessageBox::information(&dlg, "DockX", "Pick one or more docks first.");
			return;
		}
		for (QListWidgetItem *it : sel) {
			const QString key = it->data(Qt::UserRole).toString();
			if (hex.isEmpty()) {
				state().dockColorMap.remove(key);
				it->setData(Qt::ForegroundRole, QVariant());
				it->setIcon(QIcon());
			} else {
				state().dockColorMap[key] = hex;
				it->setForeground(QBrush(QColor(hex)));
				it->setIcon(colorDot(QColor(hex)));
			}
		}
		stateSave();
		panels::applyDockColors();
	};

	/* the Docks box reads as SECTIONS, not one pile of buttons: every styling
	   move gets a divider + bold heading + one dim line (Joey 2026-09-09:
	   background/fade controls blended into the dock colors) */
	auto dockSection = [dockTab, dv](const QString &title) {
		dv->addSpacing(8);
		QFrame *line = new QFrame(dockTab);
		line->setFrameShape(QFrame::HLine);
		line->setFrameShadow(QFrame::Sunken);
		dv->addWidget(line);
		QLabel *head = new QLabel(title, dockTab);
		QFont f = head->font();
		f.setBold(true);
		head->setFont(f);
		dv->addWidget(head);
	};

	dv->addWidget(groupSub("Pick one or more docks (Ctrl click or Shift click), then style them "
			       "with the sections below.",
			       dockTab));

	dockSection("Border and title color");
	addPaletteRow(dockTab, dv, &dlg, setDockColor);
	QCheckBox *glowCb = new QCheckBox("Glow on hover", dockTab);
	glowCb->setChecked(state().dockGlow);
	glowCb->setToolTip("A colored dock brightens its border while your mouse is over it. "
			   "Docks without a color have nothing to glow.");
	QObject::connect(glowCb, &QCheckBox::toggled, dockTab, [](bool on) {
		state().dockGlow = on;
		stateSave();
		panels::applyDockColors();
	});
	dv->addWidget(glowCb);
	dv->addWidget(groupSub("A colored border and title bar so you can spot the dock instantly. "
			       "Glow brightens that border under your mouse.",
			       dockTab));

	auto selectedDockKeys = [dockListW, &dlg]() -> QStringList {
		QStringList keys;
		for (QListWidgetItem *it : dockListW->selectedItems())
			keys << it->data(Qt::UserRole).toString();
		if (keys.isEmpty())
			QMessageBox::information(&dlg, "DockX", "Pick one or more docks first.");
		return keys;
	};

	dockSection("Background color");
	QHBoxLayout *exRow = new QHBoxLayout();
	QPushButton *bgBtn = new QPushButton("Set background", dockTab);
	QPushButton *bgClearBtn = new QPushButton("Clear background", dockTab);
	exRow->addWidget(bgBtn);
	exRow->addWidget(bgClearBtn);
	exRow->addStretch(1);
	dv->addLayout(exRow);
	dv->addWidget(groupSub("Tints the dock's content to match. It won't show on video docks; if "
			       "text gets hard to read, pick a darker tint or clear it.",
			       dockTab));

	QObject::connect(bgBtn, &QPushButton::clicked, &dlg, [&dlg, selectedDockKeys]() {
		const QStringList keys = selectedDockKeys();
		if (keys.isEmpty())
			return;
		const QColor c = QColorDialog::getColor(QColor("#1e1e2e"), &dlg, "Dock background");
		if (!c.isValid())
			return;
		for (const QString &k : keys)
			state().dockBgMap[k] = c.name();
		stateSave();
		panels::applyDockColors();
	});
	QObject::connect(bgClearBtn, &QPushButton::clicked, &dlg, [selectedDockKeys]() {
		const QStringList keys = selectedDockKeys();
		if (keys.isEmpty())
			return;
		for (const QString &k : keys)
			state().dockBgMap.remove(k);
		stateSave();
		panels::applyDockColors();
	});
	dockSection("Title fade");
	QHBoxLayout *gradRow = new QHBoxLayout();
	QPushButton *gradBtn = new QPushButton("Fade the title", dockTab);
	QPushButton *gradClearBtn = new QPushButton("Solid title", dockTab);
	QCheckBox *animCb = new QCheckBox("Shimmer the fades", dockTab);
	animCb->setChecked(state().gradAnimate);
	animCb->setToolTip("Faded title bars slowly swap their two colors back and forth. "
			   "Pure flair; turn it off any time.");
	QObject::connect(animCb, &QCheckBox::toggled, dockTab, [](bool on) {
		state().gradAnimate = on;
		stateSave();
		panels::applyDockColors();
	});
	gradRow->addWidget(gradBtn);
	gradRow->addWidget(gradClearBtn);
	gradRow->addWidget(animCb);
	gradRow->addStretch(1);
	dv->addLayout(gradRow);
	dv->addWidget(groupSub("Blends the title bar from the dock's color into a second color you "
			       "pick, so give the dock a color first. Shimmer slowly rocks the "
			       "blend; Solid title takes the fade off.",
			       dockTab));
	QObject::connect(gradBtn, &QPushButton::clicked, &dlg, [&dlg, selectedDockKeys]() {
		const QStringList keys = selectedDockKeys();
		if (keys.isEmpty())
			return;
		QStringList colored;
		for (const QString &k : keys)
			if (!state().dockColorMap.value(k).isEmpty())
				colored << k;
		if (colored.isEmpty()) {
			QMessageBox::information(&dlg, "DockX",
						 "Give the dock a color first; the fade blends from that "
						 "color into the one you pick next.");
			return;
		}
		const QColor c = QColorDialog::getColor(QColor("#8c1eff"), &dlg, "Fade the title into this color");
		if (!c.isValid())
			return;
		for (const QString &k : colored)
			state().dockGradMap[k] = c.name();
		stateSave();
		panels::applyDockColors();
	});
	QObject::connect(gradClearBtn, &QPushButton::clicked, &dlg, [selectedDockKeys]() {
		const QStringList keys = selectedDockKeys();
		if (keys.isEmpty())
			return;
		for (const QString &k : keys)
			state().dockGradMap.remove(k);
		stateSave();
		panels::applyDockColors();
	});

	dockSection("Lines between docks");

	QHBoxLayout *sepRow = new QHBoxLayout();
	sepRow->addWidget(new QLabel("Thickness:", dockTab));
	QSpinBox *sepSpin = new QSpinBox(dockTab);
	sepSpin->setRange(0, 12);
	sepSpin->setSpecialValueText("Theme default");
	sepSpin->setSuffix(" px");
	sepSpin->setValue(state().sepSize);
	QObject::connect(sepSpin, &QSpinBox::valueChanged, dockTab, [](int v) {
		state().sepSize = v;
		stateSave();
		panels::applySeparators();
	});
	sepRow->addWidget(sepSpin);
	QPushButton *sepColorBtn = new QPushButton("Line color", dockTab);
	QObject::connect(sepColorBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		QColor c = QColorDialog::getColor(Qt::white, &dlg, "Pick a line color");
		if (!c.isValid())
			return;
		state().sepColor = c.name();
		stateSave();
		panels::applySeparators();
	});
	sepRow->addWidget(sepColorBtn);
	QPushButton *sepNoneBtn = new QPushButton("No tint", dockTab);
	QObject::connect(sepNoneBtn, &QPushButton::clicked, dockTab, []() {
		state().sepColor.clear();
		stateSave();
		panels::applySeparators();
	});
	sepRow->addWidget(sepNoneBtn);
	sepRow->addStretch(1);
	dv->addLayout(sepRow);

	dv->addWidget(groupSub("Thicker lines make dock edges easier to see and grab.", dockTab));

	ccols->addWidget(dockTab, 1);

	/* ---------- whole window accent: tint OBS's own controls (opt in) ----------
	   built BEFORE the looks section so the look buttons can sync its checkbox;
	   added to the tab AFTER the looks box, so it sits below them visually */
	QGroupBox *chromeBox = new QGroupBox("Whole window accent", colorsTab);
	QVBoxLayout *chV = new QVBoxLayout(chromeBox);
	QHBoxLayout *chRow = new QHBoxLayout();
	QCheckBox *chromeCb = new QCheckBox("Accent OBS itself (experimental)", chromeBox);
	chromeCb->setChecked(state().chromeOn);
	chromeCb->setToolTip("Buttons, tabs, list rows, menus, sliders, scroll bars, dock titles: "
			     "one accent color over all of it. Your OBS theme stays underneath; "
			     "untick and it comes straight back.");
	QPushButton *chromeColBtn = new QPushButton("Accent color", chromeBox);
	QCheckBox *chromeAllCb = new QCheckBox("Spread to every OBS window", chromeBox);
	chromeAllCb->setChecked(state().chromeEverywhere);
	chromeAllCb->setEnabled(state().chromeOn);
	chromeAllCb->setToolTip("Also accents Settings, Properties, Filters and every other OBS "
				"window, not just the main one.");
	chRow->addWidget(chromeCb);
	chRow->addWidget(chromeColBtn);
	chRow->addWidget(chromeAllCb);
	chRow->addStretch(1);
	chV->addLayout(chRow);
	QObject::connect(chromeCb, &QCheckBox::toggled, chromeBox, [chromeAllCb](bool on) {
		state().chromeOn = on;
		chromeAllCb->setEnabled(on);
		stateSave();
		panels::applyChrome();
	});
	QObject::connect(chromeAllCb, &QCheckBox::toggled, &dlg, [&dlg, chromeAllCb](bool on) {
		if (on && QMessageBox::question(&dlg, "DockX",
						"This puts the accent on every OBS window, including Settings, "
						"Properties, and Filters. Some corners of some windows may look "
						"odd with a loud color, and you can untick this any time. "
						"Spread it?") != QMessageBox::Yes) {
			chromeAllCb->blockSignals(true);
			chromeAllCb->setChecked(false);
			chromeAllCb->blockSignals(false);
			return;
		}
		state().chromeEverywhere = on;
		stateSave();
		panels::applyChrome();
	});
	QObject::connect(chromeColBtn, &QPushButton::clicked, &dlg, [&dlg, chromeCb]() {
		const QColor c = QColorDialog::getColor(QColor(state().chromeColor), &dlg, "Whole window accent");
		if (!c.isValid())
			return;
		state().chromeColor = c.name();
		if (!chromeCb->isChecked()) {
			chromeCb->setChecked(true); /* the toggle handler saves + applies */
		} else {
			stateSave();
			panels::applyChrome();
		}
	});
	chV->addWidget(groupSub("One accent color over OBS's own controls. Applying a Look above recolors "
				"the accent to match. Chat and browser docks draw their own scroll bars "
				"(that is the website, not OBS), so those never change.",
				chromeBox));

	/* ---------- one click looks: coordinated color across every dock ---------- */
	QGroupBox *looksBox = new QGroupBox("Looks", colorsTab);
	QVBoxLayout *lkV = new QVBoxLayout(looksBox);
	QHBoxLayout *lkRow = new QHBoxLayout();
	lkV->addLayout(lkRow);

	struct Look {
		QString name;
		QStringList cols;
		QString sep;
	};
	/* the built in looks live in ONE dropdown so the row stays calm no matter
	   how many ship (Joey 2026-09-09: buttons would get overwhelming) */
	const QList<Look> looks = {
		{"StrmrX", {"#8c1eff", "#a34dff", "#6910c9", "#b975ff", "#7a14e0"}, "#8c1eff"},
		{"Synthwave", {"#ff2975", "#8c1eff", "#00e5ff", "#ff6ac1", "#5561ff"}, "#8c1eff"},
		{"Vaporwave", {"#ff71ce", "#01cdfe", "#05ffa1", "#b967ff", "#fffb96"}, "#b967ff"},
		{"Midnight ice", {"#274690", "#3e78b2", "#5aa9e6", "#4062bb", "#2b3a67"}, "#3e78b2"},
		{"Nord", {"#88c0d0", "#81a1c1", "#5e81ac", "#8fbcbb", "#b48ead"}, "#81a1c1"},
		{"Ocean", {"#0077b6", "#00b4d8", "#48cae4", "#023e8a", "#90e0ef"}, "#00b4d8"},
		{"Forest", {"#2d6a4f", "#40916c", "#52b788", "#1b4332", "#74c69d"}, "#40916c"},
		{"Sunset", {"#ff6d00", "#ff2d55", "#c9184a", "#ff9e00", "#e5383b"}, "#ff2d55"},
		{"Lava", {"#ff3d00", "#dd2c00", "#ff6e40", "#ff9e80", "#d50000"}, "#ff3d00"},
		{"Gold rush", {"#f5c518", "#d4a017", "#b8860b", "#ffdf6b", "#c9a227"}, "#d4a017"},
		{"Candy", {"#ff6b6b", "#feca57", "#48dbfb", "#ff9ff3", "#1dd1a1"}, "#feca57"},
		{"Cherry blossom", {"#ffb7c5", "#ff8fab", "#fb6f92", "#ffc2d1", "#ff4d6d"}, "#fb6f92"},
		{"Dracula", {"#bd93f9", "#ff79c6", "#8be9fd", "#50fa7b", "#ffb86c"}, "#bd93f9"},
	};
	auto applyPresetLook = [&dlg, reloadDocks, sepSpin](const Look &lk) {
		if (QMessageBox::question(&dlg, "DockX",
					  QString("Color every dock in the %1 look? Your current dock "
						  "colors are replaced (scene name colors stay).")
						  .arg(lk.name)) != QMessageBox::Yes)
			return;
		state().dockColorMap.clear();
		state().dockGradMap.clear();
		int i = 0;
		for (const panels::DockInfo &info : panels::listDocks())
			state().dockColorMap[info.key] = lk.cols[i++ % lk.cols.size()];
		state().sepColor = lk.sep;
		if (state().sepSize < 2)
			state().sepSize = 2;
		if (state().chromeOn)
			state().chromeColor = lk.sep;
		stateSave();
		panels::applyDockColors();
		panels::applySeparators();
		panels::applyChrome();
		sepSpin->blockSignals(true);
		sepSpin->setValue(state().sepSize);
		sepSpin->blockSignals(false);
		reloadDocks();
	};
	QPushButton *lkMenuBtn = new QPushButton("One click looks", looksBox);
	lkMenuBtn->setToolTip("Ready made color sets. Picking one colors every open dock at once; "
			      "each swatch shows the palette.");
	QMenu *lkMenu = new QMenu(lkMenuBtn);
	for (const Look &lk : looks) {
		QAction *a = lkMenu->addAction(paletteIcon(lk.cols), lk.name);
		QObject::connect(a, &QAction::triggered, &dlg, [applyPresetLook, lk]() { applyPresetLook(lk); });
	}
	lkMenuBtn->setMenu(lkMenu);
	lkRow->addWidget(lkMenuBtn);
	lkRow->addStretch(1);
	QPushButton *lkClear = new QPushButton("Back to theme", looksBox);
	QObject::connect(lkClear, &QPushButton::clicked, &dlg, [&dlg, reloadDocks, chromeCb]() {
		if (QMessageBox::question(&dlg, "DockX",
					  "Take every DockX color off your docks and turn off the window "
					  "accent (scene name colors stay)?") != QMessageBox::Yes)
			return;
		state().dockColorMap.clear();
		state().dockBgMap.clear();
		state().dockGradMap.clear();
		state().sepColor.clear();
		state().chromeOn = false;
		stateSave();
		panels::applyDockColors();
		panels::applySeparators();
		panels::applyChrome();
		chromeCb->setChecked(false);
		reloadDocks();
	});
	lkRow->addWidget(lkClear);

	/* ---- your saved looks: snapshot the whole Colors tab under a name ---- */
	auto quietCheck = [](QCheckBox *cb, bool on) {
		cb->blockSignals(true);
		cb->setChecked(on);
		cb->blockSignals(false);
	};
	auto applySavedLook = [&dlg, reloadDocks, chromeCb, chromeAllCb, glowCb, animCb, sepSpin, quietCheck](int id) {
		const SavedLook *found = nullptr;
		for (const SavedLook &s : state().savedLooks)
			if (s.id == id) {
				found = &s;
				break;
			}
		if (!found)
			return;
		if (QMessageBox::question(&dlg, "DockX",
					  QString("Switch every DockX color to your \"%1\" look? "
						  "(Scene name colors stay.)")
						  .arg(found->name)) != QMessageBox::Yes)
			return;
		const SavedLook lk = *found;
		state().dockColorMap = lk.dockColorMap;
		state().dockBgMap = lk.dockBgMap;
		state().dockGradMap = lk.dockGradMap;
		state().dockGlow = lk.dockGlow;
		state().gradAnimate = lk.gradAnimate;
		state().sepSize = lk.sepSize;
		state().sepColor = lk.sepColor;
		state().chromeOn = lk.chromeOn;
		state().chromeEverywhere = lk.chromeEverywhere;
		if (!lk.chromeColor.isEmpty())
			state().chromeColor = lk.chromeColor;
		stateSave();
		panels::applyDockColors();
		panels::applySeparators();
		panels::applyChrome();
		quietCheck(chromeCb, lk.chromeOn);
		quietCheck(chromeAllCb, lk.chromeEverywhere);
		chromeAllCb->setEnabled(lk.chromeOn);
		quietCheck(glowCb, lk.dockGlow);
		quietCheck(animCb, lk.gradAnimate);
		sepSpin->blockSignals(true);
		sepSpin->setValue(lk.sepSize);
		sepSpin->blockSignals(false);
		reloadDocks();
	};

	QHBoxLayout *myLkRow = new QHBoxLayout();
	QPushButton *saveLookBtn = new QPushButton("Save this look", looksBox);
	makePrimary(saveLookBtn);
	saveLookBtn->setToolTip("Keeps everything on this tab (dock colors, backgrounds, title fades, "
				"dock lines, the window accent) as your own one click button.");
	myLkRow->addWidget(saveLookBtn);
	QWidget *myLkWrap = new QWidget(looksBox);
	QHBoxLayout *myLkBtns = new QHBoxLayout(myLkWrap);
	myLkBtns->setContentsMargins(0, 0, 0, 0);
	myLkRow->addWidget(myLkWrap);
	myLkRow->addStretch(1);
	lkV->addLayout(myLkRow);

	auto rebuildHolder = std::make_shared<std::function<void()>>();
	std::function<void()> *rebuildMyLooks = rebuildHolder.get();
	*rebuildHolder = [myLkBtns, myLkWrap, &dlg, applySavedLook, rebuildMyLooks]() {
		while (QLayoutItem *item = myLkBtns->takeAt(0)) {
			if (item->widget())
				item->widget()->deleteLater();
			delete item;
		}
		for (const SavedLook &s : state().savedLooks) {
			QPushButton *b = new QPushButton(s.name, myLkWrap);
			b->setToolTip("Click to switch to this look. Right click to rename or delete it.");
			const int id = s.id;
			QObject::connect(b, &QPushButton::clicked, &dlg,
					 [applySavedLook, id]() { applySavedLook(id); });
			b->setContextMenuPolicy(Qt::CustomContextMenu);
			QObject::connect(
				b, &QPushButton::customContextMenuRequested, &dlg,
				[&dlg, b, id, rebuildMyLooks](const QPoint &pos) {
					QMenu menu(b);
					QAction *ren = menu.addAction("Rename");
					QAction *del = menu.addAction("Delete");
					QAction *picked = menu.exec(b->mapToGlobal(pos));
					if (picked == ren) {
						for (SavedLook &s : state().savedLooks) {
							if (s.id != id)
								continue;
							bool ok = false;
							const QString name =
								QInputDialog::getText(&dlg, "Rename look",
										      "New name:", QLineEdit::Normal,
										      s.name, &ok)
									.trimmed();
							if (ok && !name.isEmpty()) {
								s.name = name;
								stateSave();
								(*rebuildMyLooks)();
							}
							break;
						}
					} else if (picked == del) {
						if (QMessageBox::question(&dlg, "DockX",
									  "Delete this saved look? Your docks keep "
									  "the colors they have right now.") !=
						    QMessageBox::Yes)
							return;
						auto &v = state().savedLooks;
						for (auto it = v.begin(); it != v.end(); ++it) {
							if (it->id == id) {
								v.erase(it);
								break;
							}
						}
						stateSave();
						(*rebuildMyLooks)();
					}
				});
			myLkBtns->addWidget(b);
		}
	};
	(*rebuildHolder)();

	QObject::connect(saveLookBtn, &QPushButton::clicked, &dlg, [&dlg, rebuildHolder]() {
		bool ok = false;
		const QString name =
			QInputDialog::getText(&dlg, "Save look", "Name this look:", QLineEdit::Normal, QString(), &ok)
				.trimmed();
		if (!ok || name.isEmpty())
			return;
		SavedLook *existing = nullptr;
		for (SavedLook &s : state().savedLooks)
			if (s.name.compare(name, Qt::CaseInsensitive) == 0) {
				existing = &s;
				break;
			}
		if (existing && QMessageBox::question(&dlg, "DockX",
						      QString("You already have a look called \"%1\". Replace it with "
							      "the colors you have on right now?")
							      .arg(existing->name)) != QMessageBox::Yes)
			return;
		SavedLook lk;
		lk.id = existing ? existing->id : state().nextLookId++;
		lk.name = existing ? existing->name : name;
		lk.dockColorMap = state().dockColorMap;
		lk.dockBgMap = state().dockBgMap;
		lk.dockGradMap = state().dockGradMap;
		lk.dockGlow = state().dockGlow;
		lk.gradAnimate = state().gradAnimate;
		lk.sepSize = state().sepSize;
		lk.sepColor = state().sepColor;
		lk.chromeOn = state().chromeOn;
		lk.chromeEverywhere = state().chromeEverywhere;
		lk.chromeColor = state().chromeColor;
		if (existing)
			*existing = lk;
		else
			state().savedLooks.push_back(lk);
		stateSave();
		(*rebuildHolder)();
	});

	lkV->addWidget(groupSub("One click looks color every open dock at once, plus the dock lines. "
				"Save this look keeps your whole current setup as a button of your own; "
				"Back to theme wipes it all off.",
				looksBox));

	cvRoot->addWidget(looksBox);
	cvRoot->addWidget(chromeBox);

	/* third tab by Joey's order: Find, Layouts, Colors, then the rest */
	tabs->insertTab(2, colorsTab, "Colors");

	/* ---------- Video docks tab (source docks + the DockX Preview) ----------
	   renamed from "Source docks" + rebuilt on the de-wording recipe (Joey
	   2026-09-09: "terribly confusing as a new user"): rows say what they are,
	   the empty list teaches, and each add choice is its own explained box */
	QWidget *sdTab = new QWidget();
	QVBoxLayout *sdv = new QVBoxLayout(sdTab);

	sdv->addWidget(groupSub("Small live video windows you can dock anywhere in your layout, each "
				"showing one thing.",
				sdTab));

	QListWidget *sdListW = new HintList("No video docks yet.\n\nAdd one below: keep an eye on a camera "
					    "or chat, watch exactly what your viewers see, or add the DockX "
					    "Preview and take control of your whole layout.",
					    sdTab);
	sdListW->setToolTip("Every video dock also appears in OBS's Docks menu, and its position saves "
			    "with your dock layouts.");
	auto sdTitle = [](const SourceDockEntry &e) {
		if (e.kind == sourcedocks::KIND_PROGRAM)
			return QString("Program (what your viewers see)");
		if (e.kind == sourcedocks::KIND_PREVIEW)
			return QString("Preview (studio mode staging)");
		obs_source_t *s = obs_get_source_by_name(e.sourceName.toUtf8().constData());
		const bool isScene = s && obs_source_is_scene(s);
		obs_source_release(s);
		return QString("%1 (%2)").arg(e.sourceName, isScene ? "scene" : "source");
	};
	auto sdReload = [sdListW, sdTitle]() {
		sdListW->clear();
		for (const SourceDockEntry &e : state().sourceDocks) {
			QListWidgetItem *it = new QListWidgetItem(sdTitle(e), sdListW);
			it->setData(Qt::UserRole, e.id);
			it->setData(Qt::UserRole + 1, false); /* not an editable dock */
		}
		for (int id : editpreview::dockIds()) {
			QListWidgetItem *it = new QListWidgetItem("DockX Preview (your editable preview)", sdListW);
			it->setData(Qt::UserRole, id);
			it->setData(Qt::UserRole + 1, true); /* editable dock */
		}
	};
	sdReload();

	/* --- your current docks --- */
	QLabel *sdListLbl = new QLabel("Your video docks", sdTab);
	sdv->addWidget(sdListLbl);
	sdv->addWidget(sdListW, 1);

	QHBoxLayout *sdListBtns = new QHBoxLayout();
	QPushButton *sdRemove = new QPushButton("Remove selected", sdTab);
	sdRemove->setEnabled(false);
	sdListBtns->addWidget(sdRemove);
	sdListBtns->addStretch(1);
	sdv->addLayout(sdListBtns);
	QObject::connect(sdListW, &QListWidget::itemSelectionChanged, sdTab,
			 [sdListW, sdRemove]() { sdRemove->setEnabled(sdListW->currentItem() != nullptr); });
	QObject::connect(sdRemove, &QPushButton::clicked, sdTab, [sdListW, sdReload, sdRemove]() {
		QListWidgetItem *it = sdListW->currentItem();
		if (!it)
			return;
		const int id = it->data(Qt::UserRole).toInt();
		if (it->data(Qt::UserRole + 1).toBool())
			editpreview::removeDock(id);
		else
			sourcedocks::removeDock(id);
		sdReload();
		sdRemove->setEnabled(false);
	});

	/* --- add a new dock: three separate, explained choices --- */
	auto makeSearchCombo = [sdTab](const QStringList &items, const QString &placeholder) {
		QComboBox *c = new QComboBox(sdTab);
		c->addItems(items);
		c->setEditable(true);
		c->setInsertPolicy(QComboBox::NoInsert);
		c->setCurrentIndex(-1);
		c->lineEdit()->setPlaceholderText(placeholder);
		c->lineEdit()->setClearButtonEnabled(true);
		if (c->completer()) {
			c->completer()->setCompletionMode(QCompleter::PopupCompletion);
			c->completer()->setFilterMode(Qt::MatchContains);
			c->completer()->setCaseSensitivity(Qt::CaseInsensitive);
		}
		return c;
	};

	auto addNamed = [sdReload](QComboBox *c) {
		const QString name = c->currentText().trimmed();
		if (name.isEmpty())
			return;
		if (c->findText(name, Qt::MatchFixedString) < 0)
			return; /* only add a real list entry, never free text */
		sourcedocks::addDock(sourcedocks::KIND_SOURCE, name);
		sdReload();
		c->setCurrentIndex(-1);
		c->clearEditText();
	};

	QHBoxLayout *sdAddRow = new QHBoxLayout();

	/* the star: the editable preview that frees the layout */
	QGroupBox *epBox = new QGroupBox("DockX Preview", sdTab);
	QVBoxLayout *epV = new QVBoxLayout(epBox);
	QPushButton *editBtn = new QPushButton("Add DockX Preview", epBox);
	makePrimary(editBtn);
	editBtn->setToolTip("Its Show/Hide OBS preview button brings the built in preview back any time.");
	epV->addWidget(editBtn);
	epV->addWidget(groupSub("Your scene in a dock you can EDIT: drag sources with snapping. OBS's own "
				"preview is bolted to the center and every dock must fit around it; add "
				"this, hide the big preview (Settings tab), and your whole layout is "
				"yours.",
				epBox));
	epV->addStretch(1);
	sdAddRow->addWidget(epBox, 3);

	QGroupBox *watchBox = new QGroupBox("Watch one scene or source", sdTab);
	QVBoxLayout *ag = new QVBoxLayout(watchBox);

	QHBoxLayout *sceneRow = new QHBoxLayout();
	QLabel *sceneLbl = new QLabel("Scene", watchBox);
	sceneLbl->setMinimumWidth(50);
	QComboBox *sceneCombo = makeSearchCombo(sceneNames(), "Type to search scenes");
	QPushButton *addSceneBtn = new QPushButton("Add", watchBox);
	sceneRow->addWidget(sceneLbl);
	sceneRow->addWidget(sceneCombo, 1);
	sceneRow->addWidget(addSceneBtn);
	ag->addLayout(sceneRow);

	QHBoxLayout *srcRow = new QHBoxLayout();
	QLabel *sdSrcLbl = new QLabel("Source", watchBox);
	sdSrcLbl->setMinimumWidth(50);
	QComboBox *srcCombo = makeSearchCombo(dockableInputNames(), "Type to search sources");
	QPushButton *addSrcBtn = new QPushButton("Add", watchBox);
	srcRow->addWidget(sdSrcLbl);
	srcRow->addWidget(srcCombo, 1);
	srcRow->addWidget(addSrcBtn);
	ag->addLayout(srcRow);

	ag->addWidget(groupSub("A live mini window of just that one thing, like your camera or chat. "
			       "Audio sources get volume and mute; browser sources stay clickable.",
			       watchBox));
	ag->addStretch(1);
	sdAddRow->addWidget(watchBox, 4);

	QGroupBox *progBox = new QGroupBox("Your live output", sdTab);
	QVBoxLayout *pgV = new QVBoxLayout(progBox);
	QPushButton *addProgBtn = new QPushButton("Add Program dock", progBox);
	pgV->addWidget(addProgBtn);
	pgV->addWidget(groupSub("A small copy of exactly what your viewers are seeing right now.", progBox));
	pgV->addStretch(1);
	sdAddRow->addWidget(progBox, 2);

	QGroupBox *stBox = new QGroupBox("Stream health", sdTab);
	QVBoxLayout *stV = new QVBoxLayout(stBox);
	QPushButton *statsBtn = new QPushButton("Open DockX Stats", stBox);
	QObject::connect(statsBtn, &QPushButton::clicked, sdTab, []() { stats::showDock(); });
	stV->addWidget(statsBtn);
	stV->addWidget(groupSub("FPS, CPU, bitrate and dropped frames in a dock that shrinks to fit any "
				"corner (OBS's own Stats panel can't). Also in the Docks menu.",
				stBox));
	stV->addStretch(1);
	sdAddRow->addWidget(stBox, 2);

	sdv->addLayout(sdAddRow);

	QObject::connect(addSceneBtn, &QPushButton::clicked, sdTab, [addNamed, sceneCombo]() { addNamed(sceneCombo); });
	QObject::connect(sceneCombo->lineEdit(), &QLineEdit::returnPressed, sdTab,
			 [addNamed, sceneCombo]() { addNamed(sceneCombo); });
	QObject::connect(addSrcBtn, &QPushButton::clicked, sdTab, [addNamed, srcCombo]() { addNamed(srcCombo); });
	QObject::connect(srcCombo->lineEdit(), &QLineEdit::returnPressed, sdTab,
			 [addNamed, srcCombo]() { addNamed(srcCombo); });
	QObject::connect(editBtn, &QPushButton::clicked, sdTab, [sdReload]() {
		editpreview::addDock();
		sdReload();
	});
	QObject::connect(addProgBtn, &QPushButton::clicked, sdTab, [sdReload]() {
		sourcedocks::addDock(sourcedocks::KIND_PROGRAM, QString());
		sdReload();
	});

	tabs->addTab(sdTab, "Video docks");

	/* ---------- Placeholders tab ---------- */
	QWidget *phTab = new QWidget();
	QVBoxLayout *phv = new QVBoxLayout(phTab);

	QLabel *phIntro =
		new QLabel("A placeholder is an empty dock that reserves a spot in your layout for a window OBS "
			   "can't own, like a TikTok Live Studio chat. Give it a label and a color, then float "
			   "the real window over it. On Windows you can go further: pin the window, and DockX "
			   "keeps it on top of OBS, sized exactly over the placeholder, following it through dock "
			   "drags, layout switches and restarts. A placeholder can also show a local image, GIF, "
			   "or looping video instead: your logo, brand art, any set dressing you want living in "
			   "your layout.",
			   phTab);
	phIntro->setWordWrap(true);
	phv->addWidget(phIntro);

	QListWidget *phList = new QListWidget(phTab);
	auto phTitle = [](const PlaceholderEntry &e) {
		QString t = e.label.isEmpty() ? QString("Placeholder") : e.label;
		if (!e.pinTitle.isEmpty())
			t += QString(" · pinned: %1").arg(e.pinTitle);
		if (!e.mediaPath.isEmpty())
			t += QString(" · showing: %1").arg(QFileInfo(e.mediaPath).fileName());
		if (e.seamless)
			t += " · seamless";
		return t;
	};
	auto phReload = [phList, phTitle]() {
		phList->clear();
		for (const PlaceholderEntry &e : state().placeholders) {
			QListWidgetItem *it = new QListWidgetItem(phTitle(e), phList);
			it->setData(Qt::UserRole, e.id);
		}
	};
	phReload();
	QLabel *phListLbl = new QLabel("Your placeholders", phTab);
	phv->addWidget(phListLbl);
	phv->addWidget(phList, 1);

	auto phSelected = [phList]() -> int {
		QListWidgetItem *it = phList->currentItem();
		return it ? it->data(Qt::UserRole).toInt() : 0;
	};

	QHBoxLayout *phBtns = new QHBoxLayout();
	QPushButton *phAdd = new QPushButton("Add placeholder", phTab);
	QPushButton *phLabelBtn = new QPushButton("Set label", phTab);
	QPushButton *phColorBtn = new QPushButton("Set color", phTab);
	QPushButton *phPinBtn = new QPushButton("Pin a window", phTab);
	QPushButton *phUnpinBtn = new QPushButton("Unpin", phTab);
	QPushButton *phSeamBtn = new QPushButton("Seamless on/off", phTab);
	QPushButton *phMediaBtn = new QPushButton("Show image/video", phTab);
	QPushButton *phMediaClearBtn = new QPushButton("Clear image/video", phTab);
	QPushButton *phRemoveBtn = new QPushButton("Remove", phTab);
	phBtns->addWidget(phAdd);
	phBtns->addWidget(phLabelBtn);
	phBtns->addWidget(phColorBtn);
	if (placeholders::pinningSupported()) {
		phBtns->addWidget(phPinBtn);
		phBtns->addWidget(phUnpinBtn);
		phBtns->addWidget(phSeamBtn);
	} else {
		phPinBtn->hide();
		phUnpinBtn->hide();
		phSeamBtn->hide();
	}
	phBtns->addWidget(phMediaBtn);
	phBtns->addWidget(phMediaClearBtn);
	phBtns->addWidget(phRemoveBtn);
	phBtns->addStretch(1);
	phv->addLayout(phBtns);

	QObject::connect(phAdd, &QPushButton::clicked, phTab, [phTab, phReload]() {
		bool ok = false;
		const QString label = QInputDialog::getText(phTab, "Add placeholder",
							    "Label (what belongs in this spot):", QLineEdit::Normal,
							    "TikTok chat", &ok);
		if (!ok)
			return;
		placeholders::addDock(label.trimmed());
		phReload();
	});
	QObject::connect(phLabelBtn, &QPushButton::clicked, phTab, [phTab, phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		QString current;
		for (const PlaceholderEntry &e : state().placeholders)
			if (e.id == id)
				current = e.label;
		bool ok = false;
		const QString label =
			QInputDialog::getText(phTab, "Placeholder label", "Label:", QLineEdit::Normal, current, &ok);
		if (!ok)
			return;
		placeholders::setLabel(id, label.trimmed());
		phReload();
	});
	QObject::connect(phColorBtn, &QPushButton::clicked, phTab, [phTab, phSelected]() {
		const int id = phSelected();
		if (!id)
			return;
		QString current;
		for (const PlaceholderEntry &e : state().placeholders)
			if (e.id == id)
				current = e.color;
		const QColor start = current.isEmpty() ? QColor("#232330") : QColor(current);
		const QColor c = QColorDialog::getColor(start, phTab, "Placeholder background");
		if (c.isValid())
			placeholders::setColor(id, c.name());
	});
	QObject::connect(phPinBtn, &QPushButton::clicked, phTab, [phTab, phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		placeholders::pinWindow(id, phTab);
		phReload();
	});
	QObject::connect(phUnpinBtn, &QPushButton::clicked, phTab, [phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		placeholders::unpinWindow(id);
		phReload();
	});
	QObject::connect(phSeamBtn, &QPushButton::clicked, phTab, [phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		for (const PlaceholderEntry &e : state().placeholders) {
			if (e.id == id) {
				placeholders::setSeamless(id, !e.seamless);
				break;
			}
		}
		phReload();
	});
	QObject::connect(phMediaBtn, &QPushButton::clicked, phTab, [phTab, phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		placeholders::chooseMedia(id, phTab);
		phReload();
	});
	QObject::connect(phMediaClearBtn, &QPushButton::clicked, phTab, [phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		placeholders::clearMedia(id);
		phReload();
	});
	QObject::connect(phRemoveBtn, &QPushButton::clicked, phTab, [phSelected, phReload]() {
		const int id = phSelected();
		if (!id)
			return;
		placeholders::removeDock(id);
		phReload();
	});

	QLabel *phHint =
		new QLabel("Tip: every option here is also one right click away on the placeholder dock itself, "
			   "and its position saves with your dock layouts like any other dock.\n\n"
			   "Good to know about pinned windows:\n"
			   "•  DockX resizes the window to fill the spot, but every app has a true minimum "
			   "size that DockX cannot override. If the window refuses to shrink, the placeholder "
			   "learns that minimum and stops you dragging the dock smaller, so the space you see is "
			   "always the space the window really fits. You can often make an app shrink further by "
			   "trimming what is inside it (closing extra panels or options in that app); adding more "
			   "can raise its minimum.\n"
			   "•  If the app's minimum changes while pinned, DockX notices and relearns within "
			   "about 15 seconds. To fix it right away, right click the placeholder, choose Reset "
			   "size limit, then drag the dock to the size you want.\n"
			   "•  Some apps (TikTok Live Studio, for one) pull their popped out panel back into "
			   "the main window whenever the app restarts. DockX remembers the exact window you "
			   "pinned by its program and size, so it will not grab the app's main window by mistake; "
			   "the spot shows Waiting until you pop the panel back out, then grabs it within a "
			   "moment. It does not matter which program you open first.\n"
			   "•  When a panel and its app share a name in the pin picker, the size on the row "
			   "tells them apart: pin the small one.\n"
			   "•  Rare edge case: if an app ever shows two windows with the same name, program, "
			   "AND size, DockX cannot tell them apart and may grab the wrong one after a restart. "
			   "The fix is quick: right click the placeholder, unpin, and pin the right window "
			   "again.\n"
			   "•  Seamless hides the pinned window's own title bar and border so it reads as "
			   "pure content living in OBS; turning it off or unpinning brings the frame right back "
			   "(so does restarting that app).\n"
			   "•  Showing an image, GIF, or video: right click the placeholder, Show an image or "
			   "video. Videos loop with the sound off. The same right click menu picks how it fills "
			   "the spot: Fit shows all of it, Fill covers the spot and crops the edges, Tile "
			   "repeats it. A spot shows a file or holds a pinned window, not both at once.",
			   phTab);
	phHint->setWordWrap(true);
	phv->addWidget(phHint);

	tabs->addTab(phTab, "Placeholders");

	/* ---------- Mixer tab ---------- */
	QWidget *mixTab = new QWidget();
	QVBoxLayout *mxv = new QVBoxLayout(mixTab);

	QListWidget *mixList = new QListWidget(mixTab);
	mixList->setDragDropMode(QAbstractItemView::InternalMove);
	mixList->setSelectionMode(QAbstractItemView::SingleSelection);
	auto mixReload = [mixList]() {
		mixList->clear();
		/* custom order first, then live mixer rows, then every audio
		   source OBS knows (so the list is never empty) */
		QStringList names = state().mixerOrder;
		for (const QString &n : panels::mixerSourceNames())
			if (!names.contains(n))
				names << n;
		for (const QString &n : audioInputNames())
			if (!names.contains(n))
				names << n;
		for (const QString &n : names)
			new QListWidgetItem(n, mixList);
	};
	mixReload();
	mxv->addWidget(mixList, 1);

	auto mixPersist = [mixList]() {
		QStringList order;
		for (int i = 0; i < mixList->count(); i++)
			order << mixList->item(i)->text();
		state().mixerOrder = order;
		stateSave();
		panels::applyMixerOrder();
	};
	QObject::connect(mixList->model(), &QAbstractItemModel::rowsMoved, mixTab, [mixPersist]() { mixPersist(); });

	QHBoxLayout *mixRow = new QHBoxLayout();
	QPushButton *mixReset = new QPushButton("Forget custom order", mixTab);
	mixRow->addWidget(mixReset);
	mixRow->addStretch(1);
	mxv->addLayout(mixRow);
	QObject::connect(mixReset, &QPushButton::clicked, mixTab, [mixReload]() {
		state().mixerOrder.clear();
		stateSave();
		mixReload();
	});

	QLabel *mixHint = new QLabel("Drag to reorder the Audio Mixer. The order sticks and reapplies "
				     "itself whenever OBS rebuilds the mixer. Forgetting the custom order "
				     "returns to OBS ordering after the next scene switch.",
				     mixTab);
	mixHint->setWordWrap(true);
	mxv->addWidget(mixHint);

	tabs->addTab(mixTab, "Mixer");

	/* ---------- Align tab (optional; toggled in Settings) ---------- */
	if (state().alignTools) {
		QWidget *alignTab = new QWidget();
		QVBoxLayout *alv = new QVBoxLayout(alignTab);

		QLabel *alIntro = new QLabel("Select two or more sources on the canvas (Ctrl click them in the "
					     "preview, or drag a box around them), then line them up or space "
					     "them out. It lines up the visible edges, so scaled, cropped, or "
					     "rotated sources still land right. Locked sources are left alone.",
					     alignTab);
		alIntro->setWordWrap(true);
		alv->addWidget(alIntro);

		auto doAlign = [&dlg](align::Op op) {
			if (align::selectedCount() < 2) {
				QMessageBox::information(&dlg, "DockX",
							 "Select at least two sources on the canvas first. "
							 "Ctrl click them in the preview, or drag a box "
							 "around them.");
				return;
			}
			align::run(op);
		};
		auto doDist = [&dlg](align::Op op) {
			if (align::selectedCount() < 3) {
				QMessageBox::information(&dlg, "DockX",
							 "Pick at least three sources to space them evenly. "
							 "The two on the ends stay put and the rest spread "
							 "out between them.");
				return;
			}
			align::run(op);
		};
		auto doCenter = [&dlg](bool h, bool v) {
			if (align::selectedCount() < 1) {
				QMessageBox::information(&dlg, "DockX", "Select a source on the canvas first.");
				return;
			}
			align::center(h, v);
		};

		QGroupBox *alignBox = new QGroupBox("Line up edges", alignTab);
		QGridLayout *alg = new QGridLayout(alignBox);
		struct AB {
			const char *label;
			align::Op op;
			int row;
			int col;
		};
		const AB ab[] = {
			{"Left", align::ALIGN_LEFT, 0, 0},      {"Center", align::ALIGN_HCENTER, 0, 1},
			{"Right", align::ALIGN_RIGHT, 0, 2},    {"Top", align::ALIGN_TOP, 1, 0},
			{"Middle", align::ALIGN_VCENTER, 1, 1}, {"Bottom", align::ALIGN_BOTTOM, 1, 2},
		};
		for (const AB &x : ab) {
			QPushButton *b = new QPushButton(x.label, alignBox);
			alg->addWidget(b, x.row, x.col);
			const align::Op op = x.op;
			QObject::connect(b, &QPushButton::clicked, alignBox, [doAlign, op]() { doAlign(op); });
		}
		alv->addWidget(alignBox);

		QGroupBox *distBox = new QGroupBox("Space evenly", alignTab);
		QHBoxLayout *dgl = new QHBoxLayout(distBox);
		QPushButton *distH = new QPushButton("Across", distBox);
		QPushButton *distV = new QPushButton("Down", distBox);
		dgl->addWidget(distH);
		dgl->addWidget(distV);
		dgl->addStretch(1);
		QObject::connect(distH, &QPushButton::clicked, distBox, [doDist]() { doDist(align::DIST_H); });
		QObject::connect(distV, &QPushButton::clicked, distBox, [doDist]() { doDist(align::DIST_V); });
		alv->addWidget(distBox);

		QGroupBox *canvasBox = new QGroupBox("Center on the canvas", alignTab);
		QHBoxLayout *cgl = new QHBoxLayout(canvasBox);
		QPushButton *cH = new QPushButton("Horizontally", canvasBox);
		QPushButton *cV = new QPushButton("Vertically", canvasBox);
		QPushButton *cB = new QPushButton("Both", canvasBox);
		cgl->addWidget(cH);
		cgl->addWidget(cV);
		cgl->addWidget(cB);
		cgl->addStretch(1);
		QObject::connect(cH, &QPushButton::clicked, canvasBox, [doCenter]() { doCenter(true, false); });
		QObject::connect(cV, &QPushButton::clicked, canvasBox, [doCenter]() { doCenter(false, true); });
		QObject::connect(cB, &QPushButton::clicked, canvasBox, [doCenter]() { doCenter(true, true); });
		alv->addWidget(canvasBox);

		QLabel *alHint = new QLabel("Center lines everything up along one line through the middle of "
					    "your selection. Space evenly keeps the two end sources put and "
					    "spreads the rest between them.",
					    alignTab);
		alHint->setWordWrap(true);
		alv->addWidget(alHint);
		alv->addStretch(1);

		tabs->addTab(alignTab, "Align");
	}

	/* ---------- Switch tab (profiles + collections, live guarded) ---------- */
	QWidget *swTab = new QWidget();
	QVBoxLayout *swv = new QVBoxLayout(swTab);
	QHBoxLayout *swCols = new QHBoxLayout();

	QVBoxLayout *profCol = new QVBoxLayout();
	profCol->addWidget(new QLabel("<b>Profiles</b> (settings: encoder, "
				      "resolution, stream keys)",
				      swTab));
	QListWidget *profList = new QListWidget(swTab);
	profCol->addWidget(profList, 1);
	QPushButton *profBtn = new QPushButton("Switch profile", swTab);
	profCol->addWidget(profBtn);
	swCols->addLayout(profCol, 1);

	QVBoxLayout *collCol = new QVBoxLayout();
	collCol->addWidget(new QLabel("<b>Scene collections</b> (your scenes "
				      "and sources)",
				      swTab));
	QListWidget *collList = new QListWidget(swTab);
	collCol->addWidget(collList, 1);
	QPushButton *collBtn = new QPushButton("Switch collection", swTab);
	collCol->addWidget(collBtn);
	swCols->addLayout(collCol, 1);

	swv->addLayout(swCols, 1);
	QLabel *swHint = new QLabel("Switching either is instant when you are offline. When you are live "
				    "or recording, DockX blocks profile changes (OBS cannot do them) and "
				    "warns before a collection change, because rebuilding scenes can "
				    "hiccup the stream.",
				    swTab);
	swHint->setWordWrap(true);
	swv->addWidget(swHint);

	auto fillNameList = [](QListWidget *list, char **names, char *current) {
		list->clear();
		const QString cur = QString::fromUtf8(current ? current : "");
		for (char **n = names; n && *n; n++) {
			const QString name = QString::fromUtf8(*n);
			QListWidgetItem *it = new QListWidgetItem(name == cur ? name + "   (current)" : name, list);
			it->setData(Qt::UserRole, name);
			if (name == cur) {
				QFont f = it->font();
				f.setBold(true);
				it->setFont(f);
			}
		}
	};
	auto reloadSwitch = [profList, collList, fillNameList]() {
		char **profiles = obs_frontend_get_profiles();
		char *curProf = obs_frontend_get_current_profile();
		fillNameList(profList, profiles, curProf);
		bfree(profiles);
		bfree(curProf);
		char **colls = obs_frontend_get_scene_collections();
		char *curColl = obs_frontend_get_current_scene_collection();
		fillNameList(collList, colls, curColl);
		bfree(colls);
		bfree(curColl);
	};
	reloadSwitch();

	auto anyOutputActive = []() {
		return obs_frontend_streaming_active() || obs_frontend_recording_active() ||
		       obs_frontend_virtualcam_active();
	};
	auto switchProfile = [&dlg, profList, reloadSwitch, anyOutputActive]() {
		QListWidgetItem *it = profList->currentItem();
		if (!it)
			return;
		if (anyOutputActive()) {
			QMessageBox::information(&dlg, "DockX",
						 "OBS cannot change profiles while you are streaming, "
						 "recording, or running the virtual camera. Stop first, "
						 "then switch.");
			return;
		}
		obs_frontend_set_current_profile(it->data(Qt::UserRole).toString().toUtf8().constData());
		reloadSwitch();
	};
	auto switchCollection = [&dlg, collList, reloadSwitch, anyOutputActive]() {
		QListWidgetItem *it = collList->currentItem();
		if (!it)
			return;
		if (anyOutputActive()) {
			const auto answer =
				QMessageBox::question(&dlg, "You are live",
						      "Switching scene collections rebuilds every scene and "
						      "can hiccup your stream or recording. Switch anyway?");
			if (answer != QMessageBox::Yes)
				return;
		}
		obs_frontend_set_current_scene_collection(it->data(Qt::UserRole).toString().toUtf8().constData());
		reloadSwitch();
	};
	QObject::connect(profBtn, &QPushButton::clicked, &dlg, switchProfile);
	QObject::connect(collBtn, &QPushButton::clicked, &dlg, switchCollection);
	QObject::connect(profList, &QListWidget::itemDoubleClicked, &dlg,
			 [switchProfile](QListWidgetItem *) { switchProfile(); });
	QObject::connect(collList, &QListWidget::itemDoubleClicked, &dlg,
			 [switchCollection](QListWidgetItem *) { switchCollection(); });

	tabs->addTab(swTab, "Switch");

	/* ---------- Monitors tab ---------- */
	QWidget *monTab = new QWidget();
	QVBoxLayout *mv = new QVBoxLayout(monTab);

	QLabel *monIntro = new QLabel("Send docks to any monitor and DockX tiles them there. Save a layout "
				      "afterward and your multi monitor setup rides along with it. If a "
				      "monitor gets unplugged, DockX brings any stranded dock back onto your "
				      "main screen so you never lose one.",
				      monTab);
	monIntro->setWordWrap(true);
	mv->addWidget(monIntro);

	QHBoxLayout *monCols = new QHBoxLayout();

	QVBoxLayout *monLeft = new QVBoxLayout();
	monLeft->addWidget(new QLabel("Monitors", monTab));
	QListWidget *monScreens = new QListWidget(monTab);
	monLeft->addWidget(monScreens, 1);
	monCols->addLayout(monLeft, 1);

	QVBoxLayout *monRight = new QVBoxLayout();
	monRight->addWidget(new QLabel("Docks (pick one or more)", monTab));
	QListWidget *monDocks = new QListWidget(monTab);
	monDocks->setSelectionMode(QAbstractItemView::ExtendedSelection);
	monRight->addWidget(monDocks, 1);
	monCols->addLayout(monRight, 1);

	mv->addLayout(monCols, 1);

	auto monReloadScreens = [monScreens]() {
		const int keep = monScreens->currentRow();
		monScreens->clear();
		for (const monitors::ScreenInfo &s : monitors::listScreens()) {
			QListWidgetItem *it = new QListWidgetItem(s.label, monScreens);
			it->setData(Qt::UserRole, s.index);
		}
		if (keep >= 0 && keep < monScreens->count())
			monScreens->setCurrentRow(keep);
		else if (monScreens->count() > 0)
			monScreens->setCurrentRow(0);
	};
	auto monReloadDocks = [monDocks]() {
		monDocks->clear();
		for (const panels::DockInfo &di : panels::listDocks()) {
			QListWidgetItem *it = new QListWidgetItem(di.title, monDocks);
			it->setData(Qt::UserRole, di.key);
		}
	};
	monReloadScreens();
	monReloadDocks();

	QHBoxLayout *monBtns = new QHBoxLayout();
	QPushButton *monSend = new QPushButton("Send docks to monitor", monTab);
	QPushButton *monRescue = new QPushButton("Rescue lost docks to this screen", monTab);
	QPushButton *monRefresh = new QPushButton("Refresh", monTab);
	monBtns->addWidget(monSend);
	monBtns->addWidget(monRescue);
	monBtns->addStretch(1);
	monBtns->addWidget(monRefresh);
	mv->addLayout(monBtns);

	QObject::connect(monRefresh, &QPushButton::clicked, monTab, [monReloadScreens, monReloadDocks]() {
		monReloadScreens();
		monReloadDocks();
	});
	QObject::connect(monSend, &QPushButton::clicked, monTab, [monScreens, monDocks]() {
		QListWidgetItem *si = monScreens->currentItem();
		if (!si)
			return;
		QStringList keys;
		const QList<QListWidgetItem *> sel = monDocks->selectedItems();
		for (QListWidgetItem *it : sel)
			keys << it->data(Qt::UserRole).toString();
		if (keys.isEmpty())
			return;
		monitors::sendDocksToScreen(keys, si->data(Qt::UserRole).toInt());
	});
	QObject::connect(monRescue, &QPushButton::clicked, monTab, [monTab]() {
		const int n = monitors::rescueStrayDocks();
		const QString msg =
			n == 0 ? QString("No off screen docks found. Everything is "
					 "already in view.")
			       : QString("Brought %1 dock%2 back onto this screen.").arg(n).arg(n == 1 ? "" : "s");
		QMessageBox::information(monTab->window(), "DockX", msg);
	});

	QLabel *monHint = new QLabel("Tip: pull a dock out of OBS by its title bar to float it, then send it "
				     "where you want. Auto rescue on an unplug can be turned off in Settings.",
				     monTab);
	monHint->setWordWrap(true);
	mv->addWidget(monHint);

	tabs->addTab(monTab, "Monitors");

	/* ---------- Settings tab ---------- */
	QWidget *settingsTab = new QWidget();
	QVBoxLayout *sv = new QVBoxLayout(settingsTab);

	auto addCheck = [settingsTab, sv](const QString &label, bool value, std::function<void(bool)> onChange) {
		QCheckBox *c = new QCheckBox(label, settingsTab);
		c->setChecked(value);
		QObject::connect(c, &QCheckBox::toggled, settingsTab, [onChange](bool v) {
			onChange(v);
			stateSave();
		});
		sv->addWidget(c);
	};

	addCheck("Flexible dock layouts (drop docks side by side to build columns)", state().nesting, [](bool v) {
		state().nesting = v;
		panels::applyNesting();
	});
	addCheck("Search bar in the Scenes panel", state().sceneSearch, [](bool v) {
		state().sceneSearch = v;
		panels::applySearchBars();
	});
	addCheck("Search bar in the Sources panel", state().sourceSearch, [](bool v) {
		state().sourceSearch = v;
		panels::applySearchBars();
	});
	addCheck("Color coded scene names", state().sceneColors, [](bool v) {
		state().sceneColors = v;
		panels::refreshSoon();
	});
	addCheck("Colored dock borders", state().dockColors, [](bool v) {
		state().dockColors = v;
		panels::applyDockColors();
	});
	addCheck("Filter hotkeys (every filter gets an on/off hotkey)", state().filterHotkeys, [](bool v) {
		state().filterHotkeys = v;
		filters::applyEnabled();
	});
	addCheck("New folder button in the Scene Folders dock (off = right click only)", state().folderNewButton,
		 [](bool v) {
			 state().folderNewButton = v;
			 folders::applySettings();
		 });
	addCheck("Nested folders (drag a folder into a folder)", state().folderNesting,
		 [](bool v) { state().folderNesting = v; });
	addCheck("Sources under scenes in the Scene Folders dock", state().folderSources, [](bool v) {
		state().folderSources = v;
		folders::rebuildSoon();
	});
	addCheck("Live scene thumbnails in the folder grid (grid view = visual browser)", state().sceneThumbs,
		 [](bool v) {
			 state().sceneThumbs = v;
			 if (!v)
				 thumbs::invalidateAll();
			 folders::rebuildSoon();
		 });
	addCheck("Align and distribute tools (off by default; adds an Align tab, reopen this "
		 "window to see it)",
		 state().alignTools, [](bool v) { state().alignTools = v; });
	addCheck("Bring stranded docks back when a monitor is unplugged (auto rescue)", state().autoRescue,
		 [](bool v) { state().autoRescue = v; });
	addCheck("Collapse the main video preview (docks take the space; your stream "
		 "keeps running)",
		 state().previewCollapsed, [settingsTab](bool v) {
			 state().previewCollapsed = v;
			 preview::apply();
			 if (v)
				 preview::offerVideoDock(settingsTab->window());
		 });
	addCheck("Pop the Missing Media cleaner at startup when files are missing", state().missingAutoPop,
		 [](bool v) { state().missingAutoPop = v; });

	QHBoxLayout *missingRow = new QHBoxLayout();
	QPushButton *missingBtn = new QPushButton("Open Missing Media cleaner", settingsTab);
	missingBtn->setToolTip("Find sources whose file is gone and remove or relink them");
	QObject::connect(missingBtn, &QPushButton::clicked, settingsTab,
			 [settingsTab]() { missing::showDialog(settingsTab->window()); });
	missingRow->addWidget(missingBtn);
	missingRow->addStretch(1);
	sv->addLayout(missingRow);

	QHBoxLayout *helpRow = new QHBoxLayout();
	QPushButton *guideBtn = new QPushButton("Dock layout guide", settingsTab);
	guideBtn->setToolTip("The illustrated walkthrough from first run");
	QObject::connect(guideBtn, &QPushButton::clicked, settingsTab,
			 [settingsTab]() { hints::showGuide(settingsTab->window()); });
	helpRow->addWidget(guideBtn);
	QPushButton *helpBtn = new QPushButton("Help and guides", settingsTab);
	helpBtn->setToolTip("Opens strmrx.com/dockx in your browser");
	QObject::connect(helpBtn, &QPushButton::clicked, settingsTab,
			 []() { QDesktopServices::openUrl(QUrl(HELP_URL)); });
	helpRow->addWidget(helpBtn);
	helpRow->addStretch(1);
	sv->addLayout(helpRow);

	sv->addStretch(1);
	QLabel *about = new QLabel(
		QString("DockX %1 · by <a href=\"https://strmrx.com\">StrmrX</a>").arg(PLUGIN_VERSION), settingsTab);
	about->setOpenExternalLinks(true);
	sv->addWidget(about);

	tabs->addTab(settingsTab, "Settings");

	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addStretch(1);
	QPushButton *closeBtn = new QPushButton("Close", &dlg);
	QObject::connect(closeBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.accept(); });
	bottom->addWidget(closeBtn);
	root->addLayout(bottom);

	if (!initialTab.isEmpty()) {
		for (int i = 0; i < tabs->count(); i++)
			if (tabs->tabText(i) == initialTab) {
				tabs->setCurrentIndex(i);
				break;
			}
	}

	/* open wide enough that every tab is visible (measured, so new tabs
	   never reintroduce the scroll arrows), capped to the screen */
	int wantWidth = tabs->tabBar()->sizeHint().width() + 48;
	QScreen *scr = main ? main->screen() : QGuiApplication::primaryScreen();
	if (scr)
		wantWidth = qMin(wantWidth, (int)(scr->availableGeometry().width() * 0.92));
	if (wantWidth > dlg.minimumWidth())
		dlg.setMinimumWidth(wantWidth);

	dlg.show();
	dlg.raise();
	dlg.activateWindow();
}

} // namespace dockx
