/*
DockX for OBS Studio (by StrmrX) -- live scene thumbnails.
GPL v2, see plugin-main.cpp for the full notice.

Renders any scene source to a small preview, cached by scene uuid, for the
folder grid's visual browser. A scene is itself an obs_source, so we can render
it to an offscreen texture and read it back -- this works for scenes that are
not the current program scene. Every GPU object is created and destroyed inside
one obs_enter/leave_graphics pair, so there is nothing left to free when the
graphics subsystem tears down at exit (the ordering trap that bit source docks).

House rule holds: if anything looks off, return a null pixmap and move on. Never
crash OBS.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <plugin-support.h>

#include <QHash>
#include <QImage>

namespace dockx {
namespace thumbs {

/* 16:9 preview; matches the usual OBS canvas so scenes are not distorted */
static const int THUMB_W = 160;
static const int THUMB_H = 90;

static QHash<QString, QPixmap> g_cache;

QSize size()
{
	return QSize(THUMB_W, THUMB_H);
}

QPixmap cached(const QString &uuid)
{
	return g_cache.value(uuid);
}

/* draw one scene into an offscreen RGBA texture, then stage it back to the CPU */
static QImage grabScene(obs_source_t *scene)
{
	const uint32_t cx = obs_source_get_width(scene);
	const uint32_t cy = obs_source_get_height(scene);
	if (cx == 0 || cy == 0)
		return QImage();

	QImage img;
	obs_enter_graphics();

	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(THUMB_W, THUMB_H, GS_RGBA);
	if (tr && stage) {
		gs_texrender_reset(tr);
		if (gs_texrender_begin(tr, THUMB_W, THUMB_H)) {
			struct vec4 clear;
			vec4_zero(&clear);
			gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
			obs_source_video_render(scene);
			gs_texrender_end(tr);

			gs_stage_texture(stage, gs_texrender_get_texture(tr));
			uint8_t *data = nullptr;
			uint32_t linesize = 0;
			if (gs_stagesurface_map(stage, &data, &linesize)) {
				img = QImage(THUMB_W, THUMB_H, QImage::Format_RGBA8888);
				for (int y = 0; y < THUMB_H; y++)
					memcpy(img.scanLine(y), data + (size_t)y * linesize, (size_t)THUMB_W * 4);
				gs_stagesurface_unmap(stage);
			}
		}
	}
	if (stage)
		gs_stagesurface_destroy(stage);
	if (tr)
		gs_texrender_destroy(tr);

	obs_leave_graphics();
	return img;
}

QPixmap render(const QString &uuid)
{
	/* never render while OBS is loading or switching collections
	   (see obsReady() in dockx.hpp: startup crash fix) */
	if (!obsReady())
		return QPixmap();
	obs_source_t *scene = obs_get_source_by_uuid(uuid.toUtf8().constData());
	if (!scene)
		return QPixmap();
	QImage img = grabScene(scene);
	obs_source_release(scene);
	if (img.isNull())
		return QPixmap();
	QPixmap pm = QPixmap::fromImage(img);
	g_cache.insert(uuid, pm);
	return pm;
}

void invalidateAll()
{
	g_cache.clear();
}

void shutdown()
{
	g_cache.clear();
}

} // namespace thumbs
} // namespace dockx
