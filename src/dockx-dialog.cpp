/*
DockX for OBS Studio (by StrmrX) -- the Tools menu dialog.
GPL v2, see plugin-main.cpp for the full notice.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QStandardItemModel>
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

#include <functional>

namespace dockx {

static QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
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
static void addPaletteRow(QWidget *tab, QVBoxLayout *v, QDialog *dlg,
			  std::function<void(const QString &)> apply)
{
	QHBoxLayout *row = new QHBoxLayout();
	for (const char *hex : PRESET_COLORS) {
		QPushButton *b = new QPushButton(tab);
		b->setFixedSize(26, 26);
		b->setStyleSheet(QString("background:%1; border-radius:5px; border:none;")
					 .arg(hex));
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
		if (name.isEmpty() ||
		    obs_key_from_name(name.toUtf8().constData()) == OBS_KEY_NONE) {
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

void showDialog()
{
	QMainWindow *main = mainWindow();
	QDialog dlg(main);
	dlg.setWindowTitle("DockX");
	dlg.setMinimumSize(880, 500); /* wide enough that every tab shows */

	QVBoxLayout *root = new QVBoxLayout(&dlg);
	QTabWidget *tabs = new QTabWidget(&dlg);
	root->addWidget(tabs);

	/* ---------- Layouts tab ---------- */
	QWidget *layoutsTab = new QWidget();
	QVBoxLayout *lv = new QVBoxLayout(layoutsTab);

	QListWidget *layoutList = new QListWidget(layoutsTab);
	lv->addWidget(layoutList, 1);

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
	QPushButton *applyBtn = new QPushButton("Apply", layoutsTab);
	QPushButton *undoBtn = new QPushButton("Undo apply", layoutsTab);
	lb->addWidget(saveBtn);
	lb->addWidget(applyBtn);
	lb->addWidget(undoBtn);
	lv->addLayout(lb);

	QHBoxLayout *lb2 = new QHBoxLayout();
	QPushButton *hotkeyBtn = new QPushButton("Set hotkey", layoutsTab);
	QPushButton *unbindBtn = new QPushButton("Remove hotkey", layoutsTab);
	QPushButton *renameBtn = new QPushButton("Rename", layoutsTab);
	QPushButton *deleteBtn = new QPushButton("Delete", layoutsTab);
	lb2->addWidget(hotkeyBtn);
	lb2->addWidget(unbindBtn);
	lb2->addWidget(renameBtn);
	lb2->addWidget(deleteBtn);
	lb2->addStretch(1);
	lv->addLayout(lb2);

	QLabel *hint = new QLabel("Set hotkey binds a key right here. A Stream Deck can press "
				  "that key for one tap layout changes. Applying a layout always "
				  "keeps an undo.",
				  layoutsTab);
	hint->setWordWrap(true);
	lv->addWidget(hint);

	QObject::connect(saveBtn, &QPushButton::clicked, &dlg, [&dlg, reloadLayouts]() {
		QMainWindow *m = mainWindow();
		if (!m)
			return;
		bool ok = false;
		QString suggested = QString("Layout %1").arg((int)state().layouts.size() + 1);
		QString name = QInputDialog::getText(&dlg, "Save layout",
						     "Name this dock layout:", QLineEdit::Normal,
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
	QObject::connect(renameBtn, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLayoutId, reloadLayouts]() {
				 int id = selectedLayoutId();
				 Layout *l = id ? findLayout(id) : nullptr;
				 if (!l)
					 return;
				 bool ok = false;
				 QString name = QInputDialog::getText(&dlg, "Rename layout",
								      "New name:",
								      QLineEdit::Normal, l->name,
								      &ok);
				 name = name.trimmed();
				 if (!ok || name.isEmpty())
					 return;
				 renameLayout(id, name);
				 reloadLayouts();
			 });
	QObject::connect(deleteBtn, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLayoutId, reloadLayouts]() {
				 int id = selectedLayoutId();
				 Layout *l = id ? findLayout(id) : nullptr;
				 if (!l)
					 return;
				 auto answer = QMessageBox::question(
					 &dlg, "Delete layout",
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
	QObject::connect(hotkeyBtn, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLayoutId, reloadLayouts]() {
				 int id = selectedLayoutId();
				 Layout *l = id ? findLayout(id) : nullptr;
				 if (!l) {
					 QMessageBox::information(&dlg, "DockX",
								  "Pick a layout first.");
					 return;
				 }
				 QKeySequence seq;
				 const int r = promptHotkey(
					 &dlg, QString("Hotkey for \"%1\"").arg(l->name), seq);
				 if (r == 0)
					 return;
				 if (r == 1 && !setLayoutHotkey(l, seq))
					 QMessageBox::information(
						 &dlg, "DockX",
						 "That key is not supported here. You can still "
						 "bind it in OBS under Settings > Hotkeys "
						 "(search for DockX).");
				 if (r == 2)
					 setLayoutHotkey(l, QKeySequence());
				 reloadLayouts();
			 });
	QObject::connect(unbindBtn, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLayoutId, reloadLayouts]() {
				 int id = selectedLayoutId();
				 Layout *l = id ? findLayout(id) : nullptr;
				 if (!l) {
					 QMessageBox::information(&dlg, "DockX",
								  "Pick a layout first.");
					 return;
				 }
				 setLayoutHotkey(l, QKeySequence());
				 reloadLayouts();
			 });

	tabs->addTab(layoutsTab, "Layouts");

	/* ---------- Auto switch tab ---------- */
	QWidget *autoTab = new QWidget();
	QVBoxLayout *av = new QVBoxLayout(autoTab);

	QLabel *aintro = new QLabel("Pair a scene with a layout. When OBS switches to that "
				    "scene, DockX rearranges your docks to match. Great for a "
				    "gameplay layout, a chatting layout, an ending layout.",
				    autoTab);
	aintro->setWordWrap(true);
	av->addWidget(aintro);

	QListWidget *ruleList = new QListWidget(autoTab);
	av->addWidget(ruleList, 1);

	auto reloadRules = [ruleList]() {
		ruleList->clear();
		QStringList scenes = state().sceneLayouts.keys();
		scenes.sort(Qt::CaseInsensitive);
		for (const QString &scene : scenes) {
			Layout *l = findLayout(state().sceneLayouts.value(scene));
			if (!l)
				continue;
			QListWidgetItem *it = new QListWidgetItem(
				QString("%1   applies   %2").arg(scene, l->name), ruleList);
			it->setData(Qt::UserRole, scene);
		}
	};
	reloadRules();

	QHBoxLayout *ab = new QHBoxLayout();
	QPushButton *addRuleBtn = new QPushButton("Pair scene with layout", autoTab);
	QPushButton *removeRuleBtn = new QPushButton("Remove pairing", autoTab);
	ab->addWidget(addRuleBtn);
	ab->addWidget(removeRuleBtn);
	ab->addStretch(1);
	av->addLayout(ab);

	QObject::connect(addRuleBtn, &QPushButton::clicked, &dlg, [&dlg, reloadRules]() {
		if (state().layouts.empty()) {
			QMessageBox::information(&dlg, "DockX",
						 "Save a layout first (Layouts tab).");
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
		state().sceneLayouts[sceneBox->currentText()] =
			layoutBox->currentData().toInt();
		stateSave();
		reloadRules();
	});
	QObject::connect(removeRuleBtn, &QPushButton::clicked, &dlg,
			 [&dlg, ruleList, reloadRules]() {
				 QListWidgetItem *it = ruleList->currentItem();
				 if (!it) {
					 QMessageBox::information(&dlg, "DockX",
								  "Pick a pairing first.");
					 return;
				 }
				 state().sceneLayouts.remove(it->data(Qt::UserRole).toString());
				 stateSave();
				 reloadRules();
			 });

	QLabel *ahint = new QLabel("Tip: a Stream Deck button that switches the scene will pull "
				   "the matching layout with it automatically.",
				   autoTab);
	ahint->setWordWrap(true);
	av->addWidget(ahint);

	tabs->addTab(autoTab, "Auto switch");

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

	QObject::connect(filterUnbindBtn, &QPushButton::clicked, &dlg,
			 [&dlg, filterListW, reloadFilters]() {
				 QListWidgetItem *it = filterListW->currentItem();
				 if (!it) {
					 QMessageBox::information(&dlg, "DockX",
								  "Pick a filter first.");
					 return;
				 }
				 applyHotkeyBinding(
					 (obs_hotkey_id)it->data(Qt::UserRole).toULongLong(),
					 QKeySequence());
				 reloadFilters();
			 });

	QObject::connect(filterKeyBtn, &QPushButton::clicked, &dlg,
			 [&dlg, filterListW, reloadFilters]() {
				 QListWidgetItem *it = filterListW->currentItem();
				 if (!it) {
					 QMessageBox::information(&dlg, "DockX",
								  "Pick a filter first.");
					 return;
				 }
				 const obs_hotkey_id id =
					 (obs_hotkey_id)it->data(Qt::UserRole).toULongLong();
				 QKeySequence seq;
				 const int r = promptHotkey(&dlg, "Filter hotkey", seq);
				 if (r == 0)
					 return;
				 if (r == 1 && !applyHotkeyBinding(id, seq))
					 QMessageBox::information(
						 &dlg, "DockX",
						 "That key is not supported here. You can still "
						 "bind it in OBS under Settings > Hotkeys.");
				 if (r == 2)
					 applyHotkeyBinding(id, QKeySequence());
				 reloadFilters();
			 });

	QLabel *fhint = new QLabel(
		"Every filter on every source gets its own on/off hotkey, saved with your "
		"scene collection. Bind keys here, and press them live (or from a Stream "
		"Deck) to toggle the filter.",
		filtersTab);
	fhint->setWordWrap(true);
	fv->addWidget(fhint);

	tabs->addTab(filtersTab, "Filters");

	/* ---------- Scene colors tab ---------- */
	QWidget *colorsTab = new QWidget();
	QVBoxLayout *cv = new QVBoxLayout(colorsTab);

	QListWidget *sceneListW = new QListWidget(colorsTab);
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
		QListWidgetItem *it = sceneListW->currentItem();
		if (!it) {
			QMessageBox::information(&dlg, "DockX", "Pick a scene first.");
			return;
		}
		if (hex.isEmpty()) {
			state().colors.remove(it->text());
			it->setData(Qt::ForegroundRole, QVariant());
			it->setIcon(QIcon());
		} else {
			state().colors[it->text()] = hex;
			it->setForeground(QBrush(QColor(hex)));
			it->setIcon(colorDot(QColor(hex)));
		}
		stateSave();
		panels::refreshSoon();
	};

	addPaletteRow(colorsTab, cv, &dlg, setSceneColor);

	QLabel *chint = new QLabel("Pick a scene, then a color. The scene name shows in that "
				   "color in the Scenes panel. Sources already have this built "
				   "into OBS: right click a source and pick Set Color.",
				   colorsTab);
	chint->setWordWrap(true);
	cv->addWidget(chint);

	tabs->addTab(colorsTab, "Scene colors");

	/* ---------- Dock colors tab ---------- */
	QWidget *dockTab = new QWidget();
	QVBoxLayout *dv = new QVBoxLayout(dockTab);

	QListWidget *dockListW = new QListWidget(dockTab);
	for (const panels::DockInfo &info : panels::listDocks()) {
		QListWidgetItem *it = new QListWidgetItem(info.title, dockListW);
		it->setData(Qt::UserRole, info.key);
		const QString hex = state().dockColorMap.value(info.key);
		if (!hex.isEmpty()) {
			it->setForeground(QBrush(QColor(hex)));
			it->setIcon(colorDot(QColor(hex)));
		}
	}
	dv->addWidget(dockListW, 1);

	auto setDockColor = [dockListW, &dlg](const QString &hex) {
		QListWidgetItem *it = dockListW->currentItem();
		if (!it) {
			QMessageBox::information(&dlg, "DockX", "Pick a dock first.");
			return;
		}
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
		stateSave();
		panels::applyDockColors();
	};

	addPaletteRow(dockTab, dv, &dlg, setDockColor);

	QLabel *dhint = new QLabel("Pick a dock, then a color. The dock gets a colored border "
				   "and title bar so you can spot it instantly.",
				   dockTab);
	dhint->setWordWrap(true);
	dv->addWidget(dhint);

	QHBoxLayout *sepRow = new QHBoxLayout();
	sepRow->addWidget(new QLabel("Lines between docks:", dockTab));
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

	QLabel *sephint = new QLabel("Thicker lines make dock edges easier to see and grab.",
				     dockTab);
	sephint->setWordWrap(true);
	dv->addWidget(sephint);

	tabs->addTab(dockTab, "Dock colors");

	/* ---------- Source docks tab ---------- */
	QWidget *sdTab = new QWidget();
	QVBoxLayout *sdv = new QVBoxLayout(sdTab);

	QListWidget *sdListW = new QListWidget(sdTab);
	auto sdTitle = [](const SourceDockEntry &e) {
		if (e.kind == sourcedocks::KIND_PROGRAM)
			return QString("Program");
		if (e.kind == sourcedocks::KIND_PREVIEW)
			return QString("Preview");
		return e.sourceName;
	};
	auto sdReload = [sdListW, sdTitle]() {
		sdListW->clear();
		for (const SourceDockEntry &e : state().sourceDocks) {
			QListWidgetItem *it = new QListWidgetItem(sdTitle(e), sdListW);
			it->setData(Qt::UserRole, e.id);
		}
	};
	sdReload();
	sdv->addWidget(sdListW, 1);

	QHBoxLayout *sdRow = new QHBoxLayout();
	QComboBox *sdCombo = new QComboBox(sdTab);
	auto addHeader = [sdCombo](const QString &text) {
		sdCombo->addItem(text, -1);
		auto *m = qobject_cast<QStandardItemModel *>(sdCombo->model());
		if (m)
			m->item(sdCombo->count() - 1)->setEnabled(false);
	};
	sdCombo->addItem("Program (main output)", (int)sourcedocks::KIND_PROGRAM);
	sdCombo->addItem("Preview (studio mode)", (int)sourcedocks::KIND_PREVIEW);
	addHeader("--- Scenes ---");
	for (const QString &n : sceneNames())
		sdCombo->addItem(n, (int)sourcedocks::KIND_SOURCE);
	addHeader("--- Sources ---");
	for (const QString &n : dockableInputNames())
		sdCombo->addItem(n, (int)sourcedocks::KIND_SOURCE);
	/* type to search */
	sdCombo->setEditable(true);
	sdCombo->setInsertPolicy(QComboBox::NoInsert);
	if (sdCombo->completer()) {
		sdCombo->completer()->setCompletionMode(QCompleter::PopupCompletion);
		sdCombo->completer()->setFilterMode(Qt::MatchContains);
		sdCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
	}
	sdRow->addWidget(sdCombo, 1);
	QPushButton *sdAdd = new QPushButton("Add dock", sdTab);
	QPushButton *sdRemove = new QPushButton("Remove", sdTab);
	sdRow->addWidget(sdAdd);
	sdRow->addWidget(sdRemove);
	sdv->addLayout(sdRow);

	QObject::connect(sdAdd, &QPushButton::clicked, sdTab, [sdCombo, sdReload]() {
		const int idx = sdCombo->findText(sdCombo->currentText());
		if (idx < 0)
			return;
		const int kind = sdCombo->itemData(idx).toInt();
		if (kind < 0)
			return; /* a section header */
		const QString name = kind == sourcedocks::KIND_SOURCE
					     ? sdCombo->itemText(idx)
					     : QString();
		sourcedocks::addDock(kind, name);
		sdReload();
	});
	QObject::connect(sdRemove, &QPushButton::clicked, sdTab, [sdListW, sdReload]() {
		QListWidgetItem *it = sdListW->currentItem();
		if (!it)
			return;
		sourcedocks::removeDock(it->data(Qt::UserRole).toInt());
		sdReload();
	});

	QLabel *sdHint = new QLabel(
		"Each dock shows that source, scene, or output live. Audio sources get "
		"volume and mute controls; browser sources are clickable right in the "
		"dock. Find them in the Docks menu; their position saves with your dock "
		"layouts.",
		sdTab);
	sdHint->setWordWrap(true);
	sdv->addWidget(sdHint);

	tabs->addTab(sdTab, "Source docks");

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
	QObject::connect(mixList->model(), &QAbstractItemModel::rowsMoved, mixTab,
			 [mixPersist]() { mixPersist(); });

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

	QLabel *mixHint = new QLabel(
		"Drag to reorder the Audio Mixer. The order sticks and reapplies "
		"itself whenever OBS rebuilds the mixer. Forgetting the custom order "
		"returns to OBS ordering after the next scene switch.",
		mixTab);
	mixHint->setWordWrap(true);
	mxv->addWidget(mixHint);

	tabs->addTab(mixTab, "Mixer");

	/* ---------- Settings tab ---------- */
	QWidget *settingsTab = new QWidget();
	QVBoxLayout *sv = new QVBoxLayout(settingsTab);

	auto addCheck = [settingsTab, sv](const QString &label, bool value,
					  std::function<void(bool)> onChange) {
		QCheckBox *c = new QCheckBox(label, settingsTab);
		c->setChecked(value);
		QObject::connect(c, &QCheckBox::toggled, settingsTab, [onChange](bool v) {
			onChange(v);
			stateSave();
		});
		sv->addWidget(c);
	};

	addCheck("Flexible dock layouts (drop docks side by side to build columns)",
		 state().nesting, [](bool v) {
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
	addCheck("Filter hotkeys (every filter gets an on/off hotkey)", state().filterHotkeys,
		 [](bool v) {
			 state().filterHotkeys = v;
			 filters::applyEnabled();
		 });
	addCheck("New folder button in the Scene Folders dock (off = right click only)",
		 state().folderNewButton, [](bool v) {
			 state().folderNewButton = v;
			 folders::applySettings();
		 });
	addCheck("Nested folders (drag a folder into a folder)", state().folderNesting,
		 [](bool v) { state().folderNesting = v; });

	sv->addStretch(1);
	QLabel *about = new QLabel(
		QString("DockX %1 · by <a href=\"https://strmrx.com\">StrmrX</a>")
			.arg(PLUGIN_VERSION),
		settingsTab);
	about->setOpenExternalLinks(true);
	sv->addWidget(about);

	tabs->addTab(settingsTab, "Settings");

	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addStretch(1);
	QPushButton *closeBtn = new QPushButton("Close", &dlg);
	QObject::connect(closeBtn, &QPushButton::clicked, &dlg, [&dlg]() { dlg.accept(); });
	bottom->addWidget(closeBtn);
	root->addLayout(bottom);

	dlg.exec();
}

} // namespace dockx
