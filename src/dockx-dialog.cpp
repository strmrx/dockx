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
#include <QDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
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

static const char *PRESET_COLORS[] = {"#e5534b", "#f0883e", "#e3b341", "#57ab5a",
				      "#39c5cf", "#539bf5", "#986ee2", "#e275ad"};

void showDialog()
{
	QMainWindow *main = mainWindow();
	QDialog dlg(main);
	dlg.setWindowTitle("DockX");
	dlg.setMinimumSize(430, 420);

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
			QListWidgetItem *it = new QListWidgetItem(l.name, layoutList);
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
	QPushButton *renameBtn = new QPushButton("Rename", layoutsTab);
	QPushButton *deleteBtn = new QPushButton("Delete", layoutsTab);
	QPushButton *undoBtn = new QPushButton("Undo apply", layoutsTab);
	lb->addWidget(saveBtn);
	lb->addWidget(applyBtn);
	lb->addWidget(renameBtn);
	lb->addWidget(deleteBtn);
	lb->addWidget(undoBtn);
	lv->addLayout(lb);

	QLabel *hint = new QLabel("Hotkeys: bind a key per layout in Settings > Hotkeys "
				  "(search for DockX). Applying a layout always keeps an undo.",
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

	tabs->addTab(layoutsTab, "Layouts");

	/* ---------- Scene colors tab ---------- */
	QWidget *colorsTab = new QWidget();
	QVBoxLayout *cv = new QVBoxLayout(colorsTab);

	QListWidget *sceneListW = new QListWidget(colorsTab);
	for (const QString &name : sceneNames()) {
		QListWidgetItem *it = new QListWidgetItem(name, sceneListW);
		const QString hex = state().colors.value(name);
		if (!hex.isEmpty())
			it->setForeground(QBrush(QColor(hex)));
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
		} else {
			state().colors[it->text()] = hex;
			it->setForeground(QBrush(QColor(hex)));
		}
		stateSave();
		panels::refreshSoon();
	};

	QHBoxLayout *palette = new QHBoxLayout();
	for (const char *hex : PRESET_COLORS) {
		QPushButton *b = new QPushButton(colorsTab);
		b->setFixedSize(26, 26);
		b->setStyleSheet(QString("background:%1; border-radius:5px; border:none;")
					 .arg(hex));
		b->setToolTip("Use this color");
		QString h = QString::fromUtf8(hex);
		QObject::connect(b, &QPushButton::clicked, colorsTab,
				 [setSceneColor, h]() { setSceneColor(h); });
		palette->addWidget(b);
	}
	QPushButton *customBtn = new QPushButton("Custom", colorsTab);
	QObject::connect(customBtn, &QPushButton::clicked, &dlg, [&dlg, setSceneColor]() {
		QColor c = QColorDialog::getColor(Qt::white, &dlg, "Pick a scene color");
		if (c.isValid())
			setSceneColor(c.name());
	});
	QPushButton *noneBtn = new QPushButton("No color", colorsTab);
	QObject::connect(noneBtn, &QPushButton::clicked, colorsTab,
			 [setSceneColor]() { setSceneColor(QString()); });
	palette->addWidget(customBtn);
	palette->addWidget(noneBtn);
	palette->addStretch(1);
	cv->addLayout(palette);

	QLabel *chint = new QLabel("Pick a scene, then a color. The scene name shows in that "
				   "color in the Scenes panel. Sources already have this built "
				   "into OBS: right click a source and pick Set Color.",
				   colorsTab);
	chint->setWordWrap(true);
	cv->addWidget(chint);

	tabs->addTab(colorsTab, "Scene colors");

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
