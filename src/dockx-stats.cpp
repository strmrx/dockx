/*
DockX for OBS Studio (by StrmrX) -- the DockX Stats dock.
OBS's own Stats panel demands roughly 590px of width and jams any dock
column it sits in (its grid of labels IS its minimum size). This dock shows
the same health numbers from the same public counters, but RESPONSIVE:
wide = two pairs per line, narrow = stacked rows, tiny = essentials only.
Read only polling once a second; never touches outputs or rendering.
GPL v2, see plugin-main.cpp for the full notice.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <util/platform.h>
#include <util/config-file.h>

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace dockx {
namespace stats {

/* value coloring: empty = fine, amber = worth a glance, red = trouble */
static const char *COL_WARN = "color: #ffbe50;";
static const char *COL_BAD = "color: #ff6060;";

static QString fmtBytes(uint64_t bytes)
{
	const double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
	if (gb >= 1024.0)
		return QString::number(gb / 1024.0, 'f', 2) + " TB";
	if (gb >= 1.0)
		return QString::number(gb, 'f', 1) + " GB";
	return QString::number((double)bytes / (1024.0 * 1024.0), 'f', 0) + " MB";
}

struct Row {
	QLabel *name = nullptr;
	QLabel *value = nullptr;
	bool essential = false;
};

class StatsPanel : public QWidget {
public:
	enum { R_FPS = 0, R_CPU, R_MEM, R_DISK, R_RENDER, R_RLAG, R_ELAG, R_STREAM, R_REC, R_COUNT };

	std::vector<Row> rows;
	QWidget *content = nullptr;
	QGridLayout *grid = nullptr;
	QPushButton *resetBtn = nullptr;
	QTimer *timer = nullptr;
	os_cpu_usage_info_t *cpu = nullptr;
	int mode = -1; /* 0 = essentials, 1 = one pair per line, 2 = two pairs */

	/* counter baselines so Reset makes the percentages current again */
	uint32_t lagBase = 0, lagTotalBase = 0;
	uint32_t skipBase = 0, skipTotalBase = 0;
	int streamDropBase = 0, streamTotalBase = 0;
	bool streamWasActive = false;

	/* bitrate windows (bytes seen at the last tick) */
	uint64_t streamBytes = 0, recBytes = 0;
	uint64_t lastNs = 0;

	StatsPanel() : QWidget(nullptr)
	{
		cpu = os_cpu_usage_info_start();

		QVBoxLayout *v = new QVBoxLayout(this);
		v->setContentsMargins(0, 0, 0, 0);

		QScrollArea *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		v->addWidget(scroll);

		content = new QWidget(scroll);
		QVBoxLayout *cv = new QVBoxLayout(content);
		cv->setContentsMargins(8, 6, 8, 6);
		grid = new QGridLayout();
		grid->setHorizontalSpacing(12);
		grid->setVerticalSpacing(3);
		cv->addLayout(grid);

		QHBoxLayout *btnRow = new QHBoxLayout();
		btnRow->addStretch(1);
		resetBtn = new QPushButton("Reset", content);
		resetBtn->setToolTip("Start the lag and dropped frame counters fresh from now.");
		QObject::connect(resetBtn, &QPushButton::clicked, this, [this]() { resetBaselines(); });
		btnRow->addWidget(resetBtn);
		cv->addLayout(btnRow);
		cv->addStretch(1);
		scroll->setWidget(content);

		const struct {
			const char *name;
			bool essential;
			const char *tip;
		} defs[R_COUNT] = {
			{"FPS", true, "Frames per second OBS is rendering right now."},
			{"CPU", true, "How much of your processor OBS is using."},
			{"Memory", false, "RAM OBS is using."},
			{"Disk free", false, "Space left where recordings are saved."},
			{"Render time", false, "Average time to draw one frame. Under a few ms is healthy."},
			{"Render lag", false, "Frames missed because rendering could not keep up."},
			{"Encode lag", false, "Frames skipped because the encoder could not keep up."},
			{"Stream", true, "Live status, bitrate, and dropped frames (network)."},
			{"Recording", true, "Recording status and bitrate."},
		};
		rows.resize(R_COUNT);
		for (int i = 0; i < R_COUNT; i++) {
			rows[i].name = new QLabel(QString::fromUtf8(defs[i].name), content);
			rows[i].value = new QLabel("-", content);
			rows[i].essential = defs[i].essential;
			rows[i].name->setToolTip(QString::fromUtf8(defs[i].tip));
			rows[i].value->setToolTip(QString::fromUtf8(defs[i].tip));
			QFont f = rows[i].name->font();
			f.setBold(true);
			rows[i].name->setFont(f);
			/* let the pair shrink below its text width; clipped beats jammed */
			rows[i].value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		}
		setMinimumWidth(120);

		relayout(1);
		timer = new QTimer(this);
		timer->setInterval(1000);
		QObject::connect(timer, &QTimer::timeout, this, [this]() { tick(); });
		timer->start();
		tick();
	}

	~StatsPanel() override
	{
		if (cpu) {
			os_cpu_usage_info_destroy(cpu);
			cpu = nullptr;
		}
	}

	void resetBaselines()
	{
		lagBase = obs_get_lagged_frames();
		lagTotalBase = obs_get_total_frames();
		video_t *video = obs_get_video();
		skipBase = video ? video_output_get_skipped_frames(video) : 0;
		skipTotalBase = video ? video_output_get_total_frames(video) : 0;
		obs_output_t *so = obs_frontend_get_streaming_output();
		streamDropBase = so ? obs_output_get_frames_dropped(so) : 0;
		streamTotalBase = so ? obs_output_get_total_frames(so) : 0;
		obs_output_release(so);
		tick();
	}

	void relayout(int newMode)
	{
		if (newMode == mode)
			return;
		mode = newMode;
		while (QLayoutItem *item = grid->takeAt(0))
			delete item; /* widgets stay alive as children of content */
		int line = 0, col = 0;
		const int pairsPerLine = (mode == 2) ? 2 : 1;
		for (int i = 0; i < R_COUNT; i++) {
			const bool show = (mode > 0) || rows[i].essential;
			rows[i].name->setVisible(show);
			rows[i].value->setVisible(show);
			if (!show)
				continue;
			grid->addWidget(rows[i].name, line, col * 2);
			grid->addWidget(rows[i].value, line, col * 2 + 1);
			if (++col >= pairsPerLine) {
				col = 0;
				line++;
			}
		}
		for (int c = 0; c < pairsPerLine; c++)
			grid->setColumnStretch(c * 2 + 1, 1);
		if (mode == 2)
			grid->setColumnStretch(3, 1);
		resetBtn->setVisible(mode > 0);
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		const int w = e->size().width();
		relayout(w >= 460 ? 2 : (w >= 205 ? 1 : 0));
	}

	void setRow(int i, const QString &text, const char *style)
	{
		rows[i].value->setText(text);
		rows[i].value->setStyleSheet(QString::fromUtf8(style ? style : ""));
	}

	void tick()
	{
		if (!isVisible())
			return;
		const uint64_t now = os_gettime_ns();
		const double dt = lastNs ? (double)(now - lastNs) / 1e9 : 0.0;

		/* FPS vs the profile's target */
		obs_video_info ovi = {};
		const double target =
			obs_get_video_info(&ovi) && ovi.fps_den ? (double)ovi.fps_num / (double)ovi.fps_den : 0.0;
		const double fps = obs_get_active_fps();
		const char *fpsCol = nullptr;
		if (target > 0.0 && fps < target * 0.8)
			fpsCol = COL_BAD;
		else if (target > 0.0 && fps < target * 0.95)
			fpsCol = COL_WARN;
		setRow(R_FPS, QString::number(fps, 'f', 2), fpsCol);

		if (cpu)
			setRow(R_CPU, QString::number(os_cpu_usage_info_query(cpu), 'f', 1) + "%", nullptr);

		setRow(R_MEM, fmtBytes(os_get_proc_resident_size()), nullptr);

		/* free space where recordings land */
		config_t *cfg = obs_frontend_get_profile_config();
		const char *outMode = cfg ? config_get_string(cfg, "Output", "Mode") : nullptr;
		const bool adv = outMode && QString::fromUtf8(outMode).compare("Advanced", Qt::CaseInsensitive) == 0;
		const char *recPath =
			cfg ? config_get_string(cfg, adv ? "AdvOut" : "SimpleOutput", adv ? "RecFilePath" : "FilePath")
			    : nullptr;
		if (recPath && *recPath) {
			const uint64_t freeB = os_get_free_disk_space(recPath);
			const double gb = (double)freeB / (1024.0 * 1024.0 * 1024.0);
			setRow(R_DISK, fmtBytes(freeB), gb < 1.0 ? COL_BAD : (gb < 10.0 ? COL_WARN : nullptr));
		} else {
			setRow(R_DISK, "-", nullptr);
		}

		const double renderMs = (double)obs_get_average_frame_time_ns() / 1e6;
		setRow(R_RENDER, QString::number(renderMs, 'f', 1) + " ms", nullptr);

		auto lagText = [](uint32_t missed, uint32_t total, const char *&col) -> QString {
			col = nullptr;
			if (!total)
				return "0 / 0 (0.0%)";
			const double pct = 100.0 * (double)missed / (double)total;
			if (pct >= 5.0)
				col = COL_BAD;
			else if (pct >= 1.0)
				col = COL_WARN;
			return QString("%1 / %2 (%3%)").arg(missed).arg(total).arg(QString::number(pct, 'f', 1));
		};
		const char *col = nullptr;
		const uint32_t lag = obs_get_lagged_frames() - lagBase;
		const uint32_t lagTotal = obs_get_total_frames() - lagTotalBase;
		setRow(R_RLAG, lagText(lag, lagTotal, col), col);

		video_t *video = obs_get_video();
		const uint32_t skip = video ? video_output_get_skipped_frames(video) - skipBase : 0;
		const uint32_t skipTotal = video ? video_output_get_total_frames(video) - skipTotalBase : 0;
		setRow(R_ELAG, lagText(skip, skipTotal, col), col);

		/* stream: status + bitrate + dropped (network) */
		obs_output_t *so = obs_frontend_get_streaming_output();
		if (so && obs_output_active(so)) {
			if (!streamWasActive) { /* fresh session: counters start over */
				streamDropBase = 0;
				streamTotalBase = 0;
				streamBytes = obs_output_get_total_bytes(so);
			}
			streamWasActive = true;
			const uint64_t bytes = obs_output_get_total_bytes(so);
			const double kbps = dt > 0.0 ? (double)(bytes - streamBytes) * 8.0 / dt / 1000.0 : 0.0;
			streamBytes = bytes;
			const int dropped = obs_output_get_frames_dropped(so) - streamDropBase;
			const int total = obs_output_get_total_frames(so) - streamTotalBase;
			const double pct = total > 0 ? 100.0 * (double)dropped / (double)total : 0.0;
			const char *sCol = pct >= 5.0 ? COL_BAD : (pct >= 1.0 ? COL_WARN : nullptr);
			setRow(R_STREAM,
			       QString("Live · %1 kb/s · %2% dropped")
				       .arg(QString::number(kbps, 'f', 0), QString::number(pct, 'f', 1)),
			       sCol);
		} else {
			streamWasActive = false;
			setRow(R_STREAM, "Inactive", nullptr);
		}
		obs_output_release(so);

		obs_output_t *ro = obs_frontend_get_recording_output();
		if (ro && obs_output_active(ro)) {
			const uint64_t bytes = obs_output_get_total_bytes(ro);
			const double kbps =
				dt > 0.0 && bytes >= recBytes ? (double)(bytes - recBytes) * 8.0 / dt / 1000.0 : 0.0;
			recBytes = bytes;
			setRow(R_REC,
			       obs_frontend_recording_paused()
				       ? QString("Paused")
				       : QString("Recording · %1 kb/s").arg(QString::number(kbps, 'f', 0)),
			       nullptr);
		} else {
			recBytes = 0;
			setRow(R_REC, "Inactive", nullptr);
		}
		obs_output_release(ro);

		lastNs = now;
	}
};

static QPointer<StatsPanel> g_panel;

void createDock()
{
	StatsPanel *panel = new StatsPanel();
	if (!obs_frontend_add_dock_by_id("dockx_stats", "DockX Stats", panel)) {
		obs_log(LOG_WARNING, "could not register the DockX Stats dock");
		delete panel;
		return;
	}
	g_panel = panel;
}

void shutdown()
{
	if (g_panel && g_panel->timer)
		g_panel->timer->stop();
}

} // namespace stats
} // namespace dockx
