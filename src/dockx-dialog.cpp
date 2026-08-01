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
#include <QDesktopServices>
#include <QFileDialog>
#include <QFont>
#include <QUrl>
#include <QStandardItemModel>
#include <QDialog>
#include <QGroupBox>
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
	dlg.setMinimumSize(980, 520); /* wide enough that every tab shows */

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

	/* ---------- Loadouts tab (source positions, LoadoutX ported) ---------- */
	QWidget *loTab = new QWidget();
	QVBoxLayout *lov = new QVBoxLayout(loTab);

	QListWidget *loList = new QListWidget(loTab);
	lov->addWidget(loList, 1);

	auto reloadLoadouts = [loList]() {
		loList->clear();
		for (const SourceLoadout &l : state().loadouts) {
			const QString scope =
				l.sceneUuid.isEmpty() ? "All scenes" : l.sceneName;
			QListWidgetItem *it = new QListWidgetItem(
				QString("%1   ·   %2   ·   %3 sources")
					.arg(l.name, scope)
					.arg((int)l.items.size()),
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
				QMessageBox::information(&dlg, "DockX",
							 "No current scene to save.");
				return;
			}
			uuid = QString::fromUtf8(obs_source_get_uuid(cur));
			sceneName = QString::fromUtf8(obs_source_get_name(cur));
			obs_source_release(cur);
		}
		const QString suggested =
			allScenes ? QString("Everything %1")
					    .arg((int)state().loadouts.size() + 1)
				  : QString("%1 loadout").arg(sceneName);
		bool ok = false;
		QString name = QInputDialog::getText(&dlg, "Save loadout",
						     "Name this loadout:",
						     QLineEdit::Normal, suggested, &ok)
				       .trimmed();
		if (!ok || name.isEmpty())
			return;
		SourceLoadout l = loadouts::capture(uuid, sceneName);
		if (l.items.empty()) {
			QMessageBox::information(&dlg, "DockX",
						 "There are no sources to save yet.");
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
	QPushButton *loSaveAll = new QPushButton("Save all scenes", loTab);
	QPushButton *loRestore = new QPushButton("Restore", loTab);
	QPushButton *loUndo = new QPushButton("Undo restore", loTab);
	lob->addWidget(loSaveCur);
	lob->addWidget(loSaveAll);
	lob->addWidget(loRestore);
	lob->addWidget(loUndo);
	lov->addLayout(lob);

	QHBoxLayout *lob2 = new QHBoxLayout();
	QPushButton *loRename = new QPushButton("Rename", loTab);
	QPushButton *loDelete = new QPushButton("Delete", loTab);
	QPushButton *loExport = new QPushButton("Back up to file", loTab);
	QPushButton *loImport = new QPushButton("Import from file", loTab);
	lob2->addWidget(loRename);
	lob2->addWidget(loDelete);
	lob2->addWidget(loExport);
	lob2->addWidget(loImport);
	lob2->addStretch(1);
	lov->addLayout(lob2);

	QLabel *loHint = new QLabel(
		"A loadout remembers where every source sits: position, size, rotation, "
		"crop, visibility, and lock. Restore snaps them all back. Restoring "
		"always keeps an undo; pressing Undo restore twice flips back again. "
		"Back up to file saves your loadouts as a JSON file you can move to "
		"another PC or share; Import adds them back without overwriting anything.",
		loTab);
	loHint->setWordWrap(true);
	lov->addWidget(loHint);

	QObject::connect(loSaveCur, &QPushButton::clicked, &dlg,
			 [saveLoadout]() { saveLoadout(false); });
	QObject::connect(loSaveAll, &QPushButton::clicked, &dlg,
			 [saveLoadout]() { saveLoadout(true); });
	QObject::connect(loRestore, &QPushButton::clicked, &dlg, [&dlg, selectedLoadout]() {
		SourceLoadout *l = selectedLoadout();
		if (!l) {
			QMessageBox::information(&dlg, "DockX", "Pick a loadout first.");
			return;
		}
		const QString scope =
			l->sceneUuid.isEmpty() ? "every scene" : l->sceneName;
		const auto answer = QMessageBox::question(
			&dlg, "Restore loadout",
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
				shown << QString("and %1 more")
						 .arg(r.missing.size() - 8);
			msg += "\n\nNot found anymore:\n" + shown.join("\n");
		}
		QMessageBox::information(&dlg, "DockX", msg);
	});
	QObject::connect(loUndo, &QPushButton::clicked, &dlg, [&dlg]() {
		loadouts::RestoreReport r;
		if (!loadouts::undoRestore(r)) {
			QMessageBox::information(&dlg, "DockX", "Nothing to undo yet.");
			return;
		}
		folders::rebuildSoon();
		QMessageBox::information(
			&dlg, "DockX",
			QString("Put %1 sources back the way they were.").arg(r.restored));
	});
	QObject::connect(loRename, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLoadout, reloadLoadouts]() {
				 SourceLoadout *l = selectedLoadout();
				 if (!l)
					 return;
				 bool ok = false;
				 const QString name =
					 QInputDialog::getText(&dlg, "Rename loadout",
							       "New name:",
							       QLineEdit::Normal, l->name,
							       &ok)
						 .trimmed();
				 if (!ok || name.isEmpty())
					 return;
				 l->name = name;
				 stateSave();
				 reloadLoadouts();
			 });
	QObject::connect(loDelete, &QPushButton::clicked, &dlg,
			 [&dlg, selectedLoadout, reloadLoadouts]() {
				 SourceLoadout *l = selectedLoadout();
				 if (!l)
					 return;
				 const auto answer = QMessageBox::question(
					 &dlg, "Delete loadout",
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
	QObject::connect(loExport, &QPushButton::clicked, &dlg, [&dlg]() {
		if (state().loadouts.empty()) {
			QMessageBox::information(&dlg, "DockX",
						 "You have no loadouts to back up yet.");
			return;
		}
		const QString path = QFileDialog::getSaveFileName(
			&dlg, "Back up loadouts", "dockx-loadouts.json",
			"DockX loadouts (*.json)");
		if (path.isEmpty())
			return;
		if (loadouts::exportFile(path))
			QMessageBox::information(
				&dlg, "DockX",
				QString("Backed up %1 loadout(s). Keep this file to move "
					"them to another PC or share them.")
					.arg((int)state().loadouts.size()));
		else
			QMessageBox::warning(&dlg, "DockX",
					     "Could not write that file.");
	});
	QObject::connect(loImport, &QPushButton::clicked, &dlg, [&dlg, reloadLoadouts]() {
		const QString path = QFileDialog::getOpenFileName(
			&dlg, "Import loadouts", QString(), "DockX loadouts (*.json)");
		if (path.isEmpty())
			return;
		const int n = loadouts::importFile(path);
		if (n < 0) {
			QMessageBox::warning(
				&dlg, "DockX",
				"That file could not be read as a DockX loadouts backup.");
			return;
		}
		reloadLoadouts();
		QMessageBox::information(
			&dlg, "DockX",
			QString("Imported %1 loadout(s). They were added to your list; "
				"nothing was overwritten.")
				.arg(n));
	});

	tabs->addTab(loTab, "Loadouts");

	/* ---------- Locks tab ---------- */
	QWidget *lockTab = new QWidget();
	QVBoxLayout *lkv = new QVBoxLayout(lockTab);

	/* -- dock layout locking -- */
	QGroupBox *dockGroup = new QGroupBox("Dock layout", lockTab);
	QVBoxLayout *dg = new QVBoxLayout(dockGroup);

	QCheckBox *hardLockChk = new QCheckBox(
		"Lock docks in place (they can't be dragged or floated by accident)",
		dockGroup);
	hardLockChk->setChecked(locks::hardLock());
	dg->addWidget(hardLockChk);
	QObject::connect(hardLockChk, &QCheckBox::toggled, dockGroup,
			 [](bool on) { locks::setHardLock(on); });

	QLabel *pointLbl = new QLabel(dockGroup);
	pointLbl->setWordWrap(true);
	auto refreshPoint = [pointLbl]() {
		pointLbl->setText(
			locks::hasLockPoint()
				? "Revert point saved. If a dock drifts, snap the whole "
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
	QObject::connect(setPointBtn, &QPushButton::clicked, dockGroup,
			 [refreshPoint]() {
				 locks::setLockPoint();
				 refreshPoint();
			 });
	QObject::connect(revertBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		if (!locks::revertToLockPoint())
			QMessageBox::information(
				&dlg, "DockX",
				"Set a revert point first, then this snaps your docks "
				"back to it.");
	});

	QLabel *dockTip = new QLabel(
		"Tip: give Revert to point and the dock lock a hotkey in OBS Settings "
		"> Hotkeys (search DockX) so you can snap back mid stream without "
		"opening this window. Tools > DockX: Revert dock layout works too.",
		dockGroup);
	dockTip->setWordWrap(true);
	dg->addWidget(dockTip);
	lkv->addWidget(dockGroup);

	/* -- scene source locking -- */
	QGroupBox *srcGroup = new QGroupBox("Scene sources", lockTab);
	QVBoxLayout *sg = new QVBoxLayout(srcGroup);
	QLabel *srcLbl = new QLabel(
		"Lock every source in a scene at once so nothing on the canvas can be "
		"dragged or resized. Perfect for a Just Chatting scene you never want to "
		"nudge. This flips the same lock you see on each source, just all "
		"together.",
		srcGroup);
	srcLbl->setWordWrap(true);
	sg->addWidget(srcLbl);

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
	lkv->addWidget(srcGroup);
	lkv->addStretch(1);

	QObject::connect(lockCur, &QPushButton::clicked, &dlg, [&dlg, currentSceneName]() {
		const QString n = currentSceneName();
		locks::lockCurrentScene(true);
		QMessageBox::information(&dlg, "DockX",
					 n.isEmpty()
						 ? "Locked every source in the current scene."
						 : QString("Locked every source in \"%1\".")
							   .arg(n));
	});
	QObject::connect(unlockCur, &QPushButton::clicked, &dlg, [&dlg, currentSceneName]() {
		const QString n = currentSceneName();
		locks::lockCurrentScene(false);
		QMessageBox::information(
			&dlg, "DockX",
			n.isEmpty() ? "Unlocked every source in the current scene."
				    : QString("Unlocked every source in \"%1\".").arg(n));
	});
	QObject::connect(lockAllBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		locks::lockAllScenes(true);
		QMessageBox::information(&dlg, "DockX",
					 "Locked every source in every scene.");
	});
	QObject::connect(unlockAllBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		locks::lockAllScenes(false);
		QMessageBox::information(&dlg, "DockX",
					 "Unlocked every source in every scene.");
	});
	QObject::connect(pickBtn, &QPushButton::clicked, &dlg, [&dlg]() {
		QDialog pick(&dlg);
		pick.setWindowTitle("Lock selected scenes");
		pick.setMinimumWidth(340);
		QVBoxLayout *pv = new QVBoxLayout(&pick);
		pv->addWidget(new QLabel("Check the scenes, then lock or unlock them:",
					 &pick));
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
		QObject::connect(cancelSel, &QPushButton::clicked, &pick,
				 [&pick]() { pick.reject(); });
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
		QMessageBox::information(
			&dlg, "DockX",
			QString("%1 every source in %2 scene(s).")
				.arg(action == 1 ? "Locked" : "Unlocked")
				.arg(uuids.size()));
	});

	tabs->addTab(lockTab, "Locks");

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
	QLabel *swHint = new QLabel(
		"Switching either is instant when you are offline. When you are live "
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
			QListWidgetItem *it = new QListWidgetItem(
				name == cur ? name + "   (current)" : name, list);
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
		return obs_frontend_streaming_active() ||
		       obs_frontend_recording_active() ||
		       obs_frontend_virtualcam_active();
	};
	auto switchProfile = [&dlg, profList, reloadSwitch, anyOutputActive]() {
		QListWidgetItem *it = profList->currentItem();
		if (!it)
			return;
		if (anyOutputActive()) {
			QMessageBox::information(
				&dlg, "DockX",
				"OBS cannot change profiles while you are streaming, "
				"recording, or running the virtual camera. Stop first, "
				"then switch.");
			return;
		}
		obs_frontend_set_current_profile(
			it->data(Qt::UserRole).toString().toUtf8().constData());
		reloadSwitch();
	};
	auto switchCollection = [&dlg, collList, reloadSwitch, anyOutputActive]() {
		QListWidgetItem *it = collList->currentItem();
		if (!it)
			return;
		if (anyOutputActive()) {
			const auto answer = QMessageBox::question(
				&dlg, "You are live",
				"Switching scene collections rebuilds every scene and "
				"can hiccup your stream or recording. Switch anyway?");
			if (answer != QMessageBox::Yes)
				return;
		}
		obs_frontend_set_current_scene_collection(
			it->data(Qt::UserRole).toString().toUtf8().constData());
		reloadSwitch();
	};
	QObject::connect(profBtn, &QPushButton::clicked, &dlg, switchProfile);
	QObject::connect(collBtn, &QPushButton::clicked, &dlg, switchCollection);
	QObject::connect(profList, &QListWidget::itemDoubleClicked, &dlg,
			 [switchProfile](QListWidgetItem *) { switchProfile(); });
	QObject::connect(collList, &QListWidget::itemDoubleClicked, &dlg,
			 [switchCollection](QListWidgetItem *) { switchCollection(); });

	tabs->addTab(swTab, "Switch");

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
	addCheck("Sources under scenes in the Scene Folders dock", state().folderSources,
		 [](bool v) {
			 state().folderSources = v;
			 folders::rebuildSoon();
		 });
	addCheck("Live scene thumbnails in the folder grid (grid view = visual browser)",
		 state().sceneThumbs, [](bool v) {
			 state().sceneThumbs = v;
			 if (!v)
				 thumbs::invalidateAll();
			 folders::rebuildSoon();
		 });
	addCheck("Pop the Missing Media cleaner at startup when files are missing",
		 state().missingAutoPop, [](bool v) { state().missingAutoPop = v; });

	QHBoxLayout *missingRow = new QHBoxLayout();
	QPushButton *missingBtn =
		new QPushButton("Open Missing Media cleaner", settingsTab);
	missingBtn->setToolTip(
		"Find sources whose file is gone and remove or relink them");
	QObject::connect(missingBtn, &QPushButton::clicked, settingsTab,
			 [settingsTab]() {
				 missing::showDialog(settingsTab->window());
			 });
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
