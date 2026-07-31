/*
DockX for OBS Studio (by StrmrX) -- the Missing Media cleaner.
GPL v2, see plugin-main.cpp for the full notice.

When a source points at a file that is gone, OBS pops its own built in "Missing
Files" dialog at startup. That dialog only lets you relink the file or cancel;
there is no way to just get rid of a dead source, so OBS nags you every launch.
This adds that missing button: list every source with a missing file, then
relink it, remove the source, or remove the source and delete its file from disk.

A plugin cannot add a button to OBS's own built in dialog, so this is a separate
DockX tool. It can also pop itself at startup (off by default; see Settings).
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAbstractItemView>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <memory>
#include <vector>

namespace dockx {
namespace missing {

namespace {

QMainWindow *mainWindow()
{
	return static_cast<QMainWindow *>(obs_frontend_get_main_window());
}

/* one missing file: which source owns it, the path OBS is looking for, and the
   live handle so we can relink it (issue_callback). that handle is only valid
   while its owner obs_missing_files_t lives, so Scan holds the owners open. */
struct Row {
	QString source;
	QString path;
	obs_missing_file_t *file = nullptr;
};

struct Scan {
	std::vector<obs_missing_files_t *> owners;
	std::vector<Row> rows;

	~Scan() { clear(); }
	void clear()
	{
		for (obs_missing_files_t *o : owners)
			obs_missing_files_destroy(o);
		owners.clear();
		rows.clear();
	}
};

static bool collect(void *param, obs_source_t *source)
{
	Scan *scan = static_cast<Scan *>(param);
	obs_missing_files_t *mf = obs_source_get_missing_files(source);
	const size_t n = mf ? obs_missing_files_count(mf) : 0;
	if (n == 0) {
		if (mf)
			obs_missing_files_destroy(mf);
		return true;
	}
	const char *sname = obs_source_get_name(source);
	for (size_t i = 0; i < n; i++) {
		obs_missing_file_t *f = obs_missing_files_get_file(mf, (int)i);
		const char *p = f ? obs_missing_file_get_path(f) : nullptr;
		Row r;
		r.source = QString::fromUtf8(sname ? sname : "");
		r.path = QString::fromUtf8(p ? p : "");
		r.file = f;
		scan->rows.push_back(r);
	}
	scan->owners.push_back(mf); /* kept alive; freed in Scan::clear */
	return true;
}

static void runScan(Scan &scan)
{
	scan.clear();
	obs_enum_sources(collect, &scan);
}

/* remove a source (by name) from OBS entirely: drops it from every scene */
static void removeSource(const QString &name)
{
	obs_source_t *src = obs_get_source_by_name(name.toUtf8().constData());
	if (!src)
		return;
	obs_source_remove(src);
	obs_source_release(src);
}

} // namespace

int count()
{
	Scan scan;
	runScan(scan);
	return (int)scan.rows.size();
}

void showDialog(QWidget *parent)
{
	QWidget *p = parent ? parent : mainWindow();
	QDialog dlg(p);
	dlg.setWindowTitle("DockX: Missing media");
	dlg.resize(720, 380);

	QVBoxLayout *root = new QVBoxLayout(&dlg);

	QLabel *intro = new QLabel(
		"Sources pointing at a file that is gone. Relink one, or just get "
		"rid of it so OBS stops asking.",
		&dlg);
	intro->setWordWrap(true);
	root->addWidget(intro);

	QTableWidget *table = new QTableWidget(&dlg);
	table->setColumnCount(2);
	table->setHorizontalHeaderLabels({"Source", "Missing file"});
	table->horizontalHeader()->setStretchLastSection(true);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->verticalHeader()->setVisible(false);
	root->addWidget(table, 1);

	auto scan = std::make_shared<Scan>();

	QHBoxLayout *btnRow = new QHBoxLayout();
	QPushButton *locateBtn = new QPushButton("Relink...", &dlg);
	QPushButton *removeBtn = new QPushButton("Remove source", &dlg);
	QPushButton *removeFileBtn =
		new QPushButton("Remove source + delete file", &dlg);
	QPushButton *removeAllBtn = new QPushButton("Remove all missing", &dlg);
	locateBtn->setToolTip("Point this source at the file's new location");
	removeBtn->setToolTip("Delete this source from OBS (leaves any file alone)");
	removeFileBtn->setToolTip(
		"Delete this source and permanently delete its file from disk");
	btnRow->addWidget(locateBtn);
	btnRow->addWidget(removeBtn);
	btnRow->addWidget(removeFileBtn);
	btnRow->addStretch(1);
	btnRow->addWidget(removeAllBtn);
	root->addLayout(btnRow);

	QHBoxLayout *bottom = new QHBoxLayout();
	bottom->addStretch(1);
	QPushButton *closeBtn = new QPushButton("Close", &dlg);
	bottom->addWidget(closeBtn);
	root->addLayout(bottom);

	auto selectedRow = [table]() -> int {
		QList<QTableWidgetItem *> sel = table->selectedItems();
		if (sel.isEmpty())
			return -1;
		return sel.first()->row();
	};

	auto refreshButtons = [=]() {
		const int r = selectedRow();
		const bool has = r >= 0 && r < (int)scan->rows.size();
		locateBtn->setEnabled(has);
		removeBtn->setEnabled(has);
		removeFileBtn->setEnabled(has &&
					 QFileInfo::exists(scan->rows[r].path));
		removeAllBtn->setEnabled(!scan->rows.empty());
	};

	auto rebuild = [=]() {
		runScan(*scan);
		table->setRowCount((int)scan->rows.size());
		for (int i = 0; i < (int)scan->rows.size(); i++) {
			QTableWidgetItem *s =
				new QTableWidgetItem(scan->rows[i].source);
			QTableWidgetItem *pth =
				new QTableWidgetItem(scan->rows[i].path);
			pth->setToolTip(scan->rows[i].path);
			table->setItem(i, 0, s);
			table->setItem(i, 1, pth);
		}
		if (scan->rows.empty())
			intro->setText(
				"Nothing missing. Every source can find its file.");
		refreshButtons();
	};

	QObject::connect(table, &QTableWidget::itemSelectionChanged, &dlg,
			 [refreshButtons]() { refreshButtons(); });

	QObject::connect(locateBtn, &QPushButton::clicked, &dlg, [&, rebuild]() {
		const int r = selectedRow();
		if (r < 0 || r >= (int)scan->rows.size())
			return;
		const QString start = QFileInfo(scan->rows[r].path).absolutePath();
		const QString picked = QFileDialog::getOpenFileName(
			&dlg, "Find the file for \"" + scan->rows[r].source + "\"",
			start);
		if (picked.isEmpty())
			return;
		if (scan->rows[r].file)
			obs_missing_file_issue_callback(
				scan->rows[r].file, picked.toUtf8().constData());
		rebuild();
	});

	QObject::connect(removeBtn, &QPushButton::clicked, &dlg, [&, rebuild]() {
		const int r = selectedRow();
		if (r < 0 || r >= (int)scan->rows.size())
			return;
		const QString name = scan->rows[r].source;
		if (QMessageBox::question(
			    &dlg, "Remove source",
			    QString("Remove \"%1\" from OBS? This deletes the "
				    "source from every scene it is in.")
				    .arg(name)) != QMessageBox::Yes)
			return;
		removeSource(name);
		rebuild();
	});

	QObject::connect(removeFileBtn, &QPushButton::clicked, &dlg, [&, rebuild]() {
		const int r = selectedRow();
		if (r < 0 || r >= (int)scan->rows.size())
			return;
		const QString name = scan->rows[r].source;
		const QString path = scan->rows[r].path;
		if (!QFileInfo::exists(path)) {
			QMessageBox::information(
				&dlg, "DockX",
				"That file is already gone, so there is nothing to "
				"delete. Use Remove source instead.");
			return;
		}
		if (QMessageBox::warning(
			    &dlg, "Remove source and delete file",
			    QString("Remove \"%1\" and permanently delete this file "
				    "from your disk?\n\n%2\n\nThis cannot be undone.")
				    .arg(name, path),
			    QMessageBox::Yes | QMessageBox::No,
			    QMessageBox::No) != QMessageBox::Yes)
			return;
		removeSource(name);
		if (!QFile::remove(path))
			QMessageBox::warning(
				&dlg, "DockX",
				"Removed the source, but could not delete the file "
				"(it may be open or read only).");
		rebuild();
	});

	QObject::connect(removeAllBtn, &QPushButton::clicked, &dlg, [&, rebuild]() {
		if (scan->rows.empty())
			return;
		QStringList names; /* unique source names */
		for (const Row &row : scan->rows)
			if (!names.contains(row.source))
				names << row.source;
		if (QMessageBox::question(
			    &dlg, "Remove all missing",
			    QString("Remove %1 source(s) with missing files from "
				    "OBS? Files on disk are left alone.")
				    .arg(names.size())) != QMessageBox::Yes)
			return;
		for (const QString &n : names)
			removeSource(n);
		rebuild();
	});

	QObject::connect(closeBtn, &QPushButton::clicked, &dlg,
			 [&dlg]() { dlg.accept(); });

	rebuild();
	dlg.exec();
	/* scan and its obs_missing_files handles free here via shared_ptr dtor */
}

void autoPopIfNeeded()
{
	if (!state().missingAutoPop)
		return;
	/* defer off the FINISHED_LOADING callback so we never block startup and
	   OBS's own missing files dialog (which shows during load) is gone first */
	QTimer::singleShot(0, []() {
		if (count() > 0)
			showDialog(nullptr);
	});
}

} // namespace missing
} // namespace dockx
