/*
DockX for OBS Studio (by StrmrX) -- align + distribute tools.
GPL v2, see plugin-main.cpp for the full notice.

Lines up or evenly spaces the SELECTED sources in the current scene. Everything
works off each item's on canvas bounding box (obs_sceneitem_get_box_transform),
so scale, crop, and rotation are all respected: we line up the visible edges,
not the raw position. We only ever move an item, never resize it, so nothing is
distorted. Locked items are left where they are.
*/

#include "dockx.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>

#include <algorithm>
#include <vector>

namespace dockx {
namespace align {

namespace {

struct Box {
	obs_sceneitem_t *item;
	float left, top, right, bottom;
	float cx() const { return (left + right) / 2.0f; }
	float cy() const { return (top + bottom) / 2.0f; }
};

/* axis-aligned bounding box of a scene item on the canvas */
static Box boxOf(obs_sceneitem_t *item)
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
	const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (const auto &c : corners) {
		vec3 v, r;
		vec3_set(&v, c[0], c[1], 0.0f);
		vec3_transform(&r, &v, &m);
		minx = std::min(minx, r.x);
		miny = std::min(miny, r.y);
		maxx = std::max(maxx, r.x);
		maxy = std::max(maxy, r.y);
	}
	return {item, minx, miny, maxx, maxy};
}

struct Collect {
	std::vector<Box> *boxes;
};

static bool collectSelected(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *c = static_cast<Collect *>(param);
	if (obs_sceneitem_selected(item) && !obs_sceneitem_locked(item))
		c->boxes->push_back(boxOf(item));
	return true;
}

static std::vector<Box> selection()
{
	std::vector<Box> boxes;
	obs_source_t *scene = obs_frontend_get_current_scene();
	if (scene) {
		obs_scene_t *s = obs_scene_from_source(scene);
		if (s) {
			Collect c{&boxes};
			obs_scene_enum_items(s, collectSelected, &c);
		}
		obs_source_release(scene);
	}
	return boxes;
}

static void shift(obs_sceneitem_t *item, float dx, float dy)
{
	if (dx == 0.0f && dy == 0.0f)
		return;
	vec2 pos;
	obs_sceneitem_get_pos(item, &pos);
	pos.x += dx;
	pos.y += dy;
	obs_sceneitem_set_pos(item, &pos);
}

} // namespace

int selectedCount()
{
	return (int)selection().size();
}

void run(Op op)
{
	std::vector<Box> b = selection();
	if (b.size() < 2)
		return;

	float minL = 1e30f, minT = 1e30f, maxR = -1e30f, maxB = -1e30f;
	for (const Box &x : b) {
		minL = std::min(minL, x.left);
		minT = std::min(minT, x.top);
		maxR = std::max(maxR, x.right);
		maxB = std::max(maxB, x.bottom);
	}
	const float midX = (minL + maxR) / 2.0f;
	const float midY = (minT + maxB) / 2.0f;

	switch (op) {
	case ALIGN_LEFT:
		for (Box &x : b)
			shift(x.item, minL - x.left, 0.0f);
		break;
	case ALIGN_HCENTER:
		for (Box &x : b)
			shift(x.item, midX - x.cx(), 0.0f);
		break;
	case ALIGN_RIGHT:
		for (Box &x : b)
			shift(x.item, maxR - x.right, 0.0f);
		break;
	case ALIGN_TOP:
		for (Box &x : b)
			shift(x.item, 0.0f, minT - x.top);
		break;
	case ALIGN_VCENTER:
		for (Box &x : b)
			shift(x.item, 0.0f, midY - x.cy());
		break;
	case ALIGN_BOTTOM:
		for (Box &x : b)
			shift(x.item, 0.0f, maxB - x.bottom);
		break;
	case DIST_H: {
		if (b.size() < 3)
			break;
		std::sort(b.begin(), b.end(), [](const Box &a, const Box &c) { return a.cx() < c.cx(); });
		const float first = b.front().cx();
		const float step = (b.back().cx() - first) / (float)(b.size() - 1);
		for (size_t i = 1; i + 1 < b.size(); i++)
			shift(b[i].item, (first + step * (float)i) - b[i].cx(), 0.0f);
		break;
	}
	case DIST_V: {
		if (b.size() < 3)
			break;
		std::sort(b.begin(), b.end(), [](const Box &a, const Box &c) { return a.cy() < c.cy(); });
		const float first = b.front().cy();
		const float step = (b.back().cy() - first) / (float)(b.size() - 1);
		for (size_t i = 1; i + 1 < b.size(); i++)
			shift(b[i].item, 0.0f, (first + step * (float)i) - b[i].cy());
		break;
	}
	}
}

void center(bool horizontal, bool vertical)
{
	std::vector<Box> b = selection();
	if (b.empty())
		return;
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return;

	/* move the whole selection as a block so relative spacing is kept */
	float minL = 1e30f, minT = 1e30f, maxR = -1e30f, maxB = -1e30f;
	for (const Box &x : b) {
		minL = std::min(minL, x.left);
		minT = std::min(minT, x.top);
		maxR = std::max(maxR, x.right);
		maxB = std::max(maxB, x.bottom);
	}
	const float dx = horizontal ? ((float)ovi.base_width - (minL + maxR)) / 2.0f : 0.0f;
	const float dy = vertical ? ((float)ovi.base_height - (minT + maxB)) / 2.0f : 0.0f;
	for (Box &x : b)
		shift(x.item, dx, dy);
}

} // namespace align
} // namespace dockx
