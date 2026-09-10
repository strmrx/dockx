/*
DockX for OBS Studio (by StrmrX) -- source loadouts + lock tools.
GPL v2, see plugin-main.cpp for the full notice.

LoadoutX's headline features, done natively. A loadout snapshots every source's
transform (position/scale/rotation/bounds/crop), visibility, and lock state,
for one scene or all scenes, and restores them in one click. Restoring always
snapshots an undo first; undoing twice toggles back (a free redo).

Matching on restore is by scene item id first (stable within a collection),
with a by-name fallback so renamed collections degrade gracefully. Group
children are captured with their containing group so their group-relative
transforms round trip. Missing sources are reported, never guessed at.
*/

#include "dockx.hpp"

#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <cmath>

namespace dockx {
namespace loadouts {

/* ---- capture ---- */

static LoadoutItem captureItem(obs_sceneitem_t *item, const QString &sceneUuid, const QString &sceneName,
			       obs_sceneitem_t *group)
{
	LoadoutItem li;
	li.sceneUuid = sceneUuid;
	li.sceneName = sceneName;
	obs_source_t *src = obs_sceneitem_get_source(item);
	li.sourceName = QString::fromUtf8(src ? obs_source_get_name(src) : "");
	li.itemId = obs_sceneitem_get_id(item);
	if (group) {
		li.groupItemId = obs_sceneitem_get_id(group);
		obs_source_t *gsrc = obs_sceneitem_get_source(group);
		li.groupName = QString::fromUtf8(gsrc ? obs_source_get_name(gsrc) : "");
	}

	obs_transform_info info;
	obs_sceneitem_get_info2(item, &info);
	li.posX = info.pos.x;
	li.posY = info.pos.y;
	li.rot = info.rot;
	li.scaleX = info.scale.x;
	li.scaleY = info.scale.y;
	li.alignment = (int)info.alignment;
	li.boundsType = (int)info.bounds_type;
	li.boundsAlign = (int)info.bounds_alignment;
	li.boundsX = info.bounds.x;
	li.boundsY = info.bounds.y;
	li.cropToBounds = info.crop_to_bounds;

	obs_sceneitem_crop crop;
	obs_sceneitem_get_crop(item, &crop);
	li.cropL = crop.left;
	li.cropT = crop.top;
	li.cropR = crop.right;
	li.cropB = crop.bottom;

	li.visible = obs_sceneitem_visible(item);
	li.locked = obs_sceneitem_locked(item);
	return li;
}

struct CaptureCtx {
	SourceLoadout *out;
	QString sceneUuid;
	QString sceneName;
	obs_sceneitem_t *group = nullptr;
};

static bool captureEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	CaptureCtx *ctx = static_cast<CaptureCtx *>(param);
	ctx->out->items.push_back(captureItem(item, ctx->sceneUuid, ctx->sceneName, ctx->group));
	if (obs_sceneitem_is_group(item)) {
		CaptureCtx sub = *ctx;
		sub.group = item;
		obs_sceneitem_group_enum_items(item, captureEnum, &sub);
	}
	return true;
}

static void captureScene(SourceLoadout &out, obs_source_t *sceneSrc)
{
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	if (!scene)
		return;
	CaptureCtx ctx;
	ctx.out = &out;
	ctx.sceneUuid = QString::fromUtf8(obs_source_get_uuid(sceneSrc));
	ctx.sceneName = QString::fromUtf8(obs_source_get_name(sceneSrc));
	obs_scene_enum_items(scene, captureEnum, &ctx);
}

SourceLoadout capture(const QString &sceneUuid, const QString &sceneName)
{
	SourceLoadout out;
	out.sceneUuid = sceneUuid;
	out.sceneName = sceneName;
	if (!sceneUuid.isEmpty()) {
		obs_source_t *src = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
		if (src) {
			captureScene(out, src);
			obs_source_release(src);
		}
		return out;
	}
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++)
		captureScene(out, list.sources.array[i]);
	obs_frontend_source_list_free(&list);
	return out;
}

/* ---- restore ---- */

/* the item this record points at today, or null; id first, then name */
static obs_sceneitem_t *resolveItem(obs_scene_t *scene, const LoadoutItem &li)
{
	obs_scene_t *host = scene;
	if (li.groupItemId != 0 || !li.groupName.isEmpty()) {
		obs_sceneitem_t *group = obs_scene_find_sceneitem_by_id(scene, li.groupItemId);
		if (!group || !obs_sceneitem_is_group(group))
			group = obs_scene_get_group(scene, li.groupName.toUtf8().constData());
		if (!group)
			return nullptr;
		host = obs_sceneitem_group_get_scene(group);
		if (!host)
			return nullptr;
	}
	obs_sceneitem_t *item = obs_scene_find_sceneitem_by_id(host, li.itemId);
	if (!item)
		item = obs_scene_find_source(host, li.sourceName.toUtf8().constData());
	return item;
}

static bool applyItem(const LoadoutItem &li)
{
	obs_source_t *sceneSrc = obs_get_source_by_uuid(li.sceneUuid.toUtf8().constData());
	if (!sceneSrc)
		return false;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	obs_sceneitem_t *item = scene ? resolveItem(scene, li) : nullptr;
	if (!item) {
		obs_source_release(sceneSrc);
		return false;
	}

	obs_sceneitem_defer_update_begin(item);
	obs_transform_info info;
	info.pos.x = (float)li.posX;
	info.pos.y = (float)li.posY;
	info.rot = (float)li.rot;
	info.scale.x = (float)li.scaleX;
	info.scale.y = (float)li.scaleY;
	info.alignment = (uint32_t)li.alignment;
	info.bounds_type = (enum obs_bounds_type)li.boundsType;
	info.bounds_alignment = (uint32_t)li.boundsAlign;
	info.bounds.x = (float)li.boundsX;
	info.bounds.y = (float)li.boundsY;
	info.crop_to_bounds = li.cropToBounds;
	obs_sceneitem_set_info2(item, &info);
	obs_sceneitem_crop crop;
	crop.left = li.cropL;
	crop.top = li.cropT;
	crop.right = li.cropR;
	crop.bottom = li.cropB;
	obs_sceneitem_set_crop(item, &crop);
	obs_sceneitem_defer_update_end(item);
	obs_sceneitem_set_visible(item, li.visible);
	obs_sceneitem_set_locked(item, li.locked);

	obs_source_release(sceneSrc);
	return true;
}

static RestoreReport applyAll(const SourceLoadout &l)
{
	RestoreReport report;
	for (const LoadoutItem &li : l.items) {
		if (applyItem(li))
			report.restored++;
		else
			report.missing << QString("%1: %2").arg(li.sceneName, li.sourceName);
	}
	return report;
}

RestoreReport restore(const SourceLoadout &l)
{
	/* the safety net first: snapshot the same scope as it is right now */
	state().loadoutUndo = capture(l.sceneUuid, l.sceneName);
	state().hasLoadoutUndo = true;
	RestoreReport report = applyAll(l);
	stateSave();
	return report;
}

bool undoRestore(RestoreReport &report)
{
	if (!state().hasLoadoutUndo)
		return false;
	/* capture now, apply the old snapshot, keep the capture: undo = redo */
	SourceLoadout cur = capture(state().loadoutUndo.sceneUuid, state().loadoutUndo.sceneName);
	report = applyAll(state().loadoutUndo);
	state().loadoutUndo = cur;
	stateSave();
	return true;
}

/* ---- lock tools ---- */

struct LockCtx {
	bool locked;
};

static bool lockEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	LockCtx *ctx = static_cast<LockCtx *>(param);
	obs_sceneitem_set_locked(item, ctx->locked);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, lockEnum, param);
	return true;
}

void lockScene(const QString &sceneUuid, bool locked)
{
	obs_source_t *src = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!src)
		return;
	obs_scene_t *scene = obs_scene_from_source(src);
	if (scene) {
		LockCtx ctx{locked};
		obs_scene_enum_items(scene, lockEnum, &ctx);
	}
	obs_source_release(src);
}

void lockAll(bool locked)
{
	obs_frontend_source_list list = {};
	obs_frontend_get_scenes(&list);
	for (size_t i = 0; i < list.sources.num; i++) {
		obs_scene_t *scene = obs_scene_from_source(list.sources.array[i]);
		if (!scene)
			continue;
		LockCtx ctx{locked};
		obs_scene_enum_items(scene, lockEnum, &ctx);
	}
	obs_frontend_source_list_free(&list);
}

/* ---- (de)serialization for dockx.json ---- */

obs_data_t *toData(const SourceLoadout &l)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "id", l.id);
	obs_data_set_string(d, "name", l.name.toUtf8().constData());
	obs_data_set_string(d, "scene_uuid", l.sceneUuid.toUtf8().constData());
	obs_data_set_string(d, "scene_name", l.sceneName.toUtf8().constData());
	obs_data_array_t *items = obs_data_array_create();
	for (const LoadoutItem &li : l.items) {
		obs_data_t *e = obs_data_create();
		obs_data_set_string(e, "scene_uuid", li.sceneUuid.toUtf8().constData());
		obs_data_set_string(e, "scene_name", li.sceneName.toUtf8().constData());
		obs_data_set_string(e, "source_name", li.sourceName.toUtf8().constData());
		obs_data_set_int(e, "item_id", li.itemId);
		obs_data_set_int(e, "group_item_id", li.groupItemId);
		obs_data_set_string(e, "group_name", li.groupName.toUtf8().constData());
		obs_data_set_double(e, "pos_x", li.posX);
		obs_data_set_double(e, "pos_y", li.posY);
		obs_data_set_double(e, "rot", li.rot);
		obs_data_set_double(e, "scale_x", li.scaleX);
		obs_data_set_double(e, "scale_y", li.scaleY);
		obs_data_set_int(e, "alignment", li.alignment);
		obs_data_set_int(e, "bounds_type", li.boundsType);
		obs_data_set_int(e, "bounds_align", li.boundsAlign);
		obs_data_set_double(e, "bounds_x", li.boundsX);
		obs_data_set_double(e, "bounds_y", li.boundsY);
		obs_data_set_bool(e, "crop_to_bounds", li.cropToBounds);
		obs_data_set_int(e, "crop_l", li.cropL);
		obs_data_set_int(e, "crop_t", li.cropT);
		obs_data_set_int(e, "crop_r", li.cropR);
		obs_data_set_int(e, "crop_b", li.cropB);
		obs_data_set_bool(e, "visible", li.visible);
		obs_data_set_bool(e, "locked", li.locked);
		obs_data_array_push_back(items, e);
		obs_data_release(e);
	}
	obs_data_set_array(d, "items", items);
	obs_data_array_release(items);
	return d;
}

SourceLoadout fromData(obs_data_t *d)
{
	SourceLoadout l;
	l.id = (int)obs_data_get_int(d, "id");
	l.name = QString::fromUtf8(obs_data_get_string(d, "name"));
	l.sceneUuid = QString::fromUtf8(obs_data_get_string(d, "scene_uuid"));
	l.sceneName = QString::fromUtf8(obs_data_get_string(d, "scene_name"));
	obs_data_array_t *items = obs_data_get_array(d, "items");
	const size_t n = items ? obs_data_array_count(items) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(items, i);
		LoadoutItem li;
		li.sceneUuid = QString::fromUtf8(obs_data_get_string(e, "scene_uuid"));
		li.sceneName = QString::fromUtf8(obs_data_get_string(e, "scene_name"));
		li.sourceName = QString::fromUtf8(obs_data_get_string(e, "source_name"));
		li.itemId = obs_data_get_int(e, "item_id");
		li.groupItemId = obs_data_get_int(e, "group_item_id");
		li.groupName = QString::fromUtf8(obs_data_get_string(e, "group_name"));
		li.posX = obs_data_get_double(e, "pos_x");
		li.posY = obs_data_get_double(e, "pos_y");
		li.rot = obs_data_get_double(e, "rot");
		li.scaleX = obs_data_get_double(e, "scale_x");
		li.scaleY = obs_data_get_double(e, "scale_y");
		li.alignment = (int)obs_data_get_int(e, "alignment");
		li.boundsType = (int)obs_data_get_int(e, "bounds_type");
		li.boundsAlign = (int)obs_data_get_int(e, "bounds_align");
		li.boundsX = obs_data_get_double(e, "bounds_x");
		li.boundsY = obs_data_get_double(e, "bounds_y");
		li.cropToBounds = obs_data_get_bool(e, "crop_to_bounds");
		li.cropL = (int)obs_data_get_int(e, "crop_l");
		li.cropT = (int)obs_data_get_int(e, "crop_t");
		li.cropR = (int)obs_data_get_int(e, "crop_r");
		li.cropB = (int)obs_data_get_int(e, "crop_b");
		li.visible = obs_data_get_bool(e, "visible");
		li.locked = obs_data_get_bool(e, "locked");
		l.items.push_back(li);
	}
	if (items)
		obs_data_array_release(items);
	return l;
}

/* ---- file backup / import (portability + sharing across machines) ---- */

bool exportFile(const QString &path)
{
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "dockx_loadouts", "1"); /* format marker */
	obs_data_array_t *arr = obs_data_array_create();
	for (const SourceLoadout &l : state().loadouts) {
		obs_data_t *e = toData(l);
		obs_data_array_push_back(arr, e);
		obs_data_release(e);
	}
	obs_data_set_array(root, "loadouts", arr);
	obs_data_array_release(arr);
	bool ok = obs_data_save_json_pretty_safe(root, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(root);
	return ok;
}

int importFile(const QString &path)
{
	obs_data_t *root = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!root)
		return -1;
	obs_data_array_t *arr = obs_data_get_array(root, "loadouts");
	if (!arr) {
		obs_data_release(root);
		return -1;
	}
	int added = 0;
	const size_t n = obs_data_array_count(arr);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *e = obs_data_array_item(arr, i);
		SourceLoadout l = fromData(e);
		obs_data_release(e);
		if (l.name.isEmpty() && l.items.empty())
			continue;
		l.id = state().nextLoadoutId++; /* fresh local id; append, never clobber */
		state().loadouts.push_back(l);
		registerHotkey(state().loadouts.back());
		added++;
	}
	obs_data_array_release(arr);
	obs_data_release(root);
	if (added > 0)
		stateSave();
	return added;
}

} // namespace loadouts
} // namespace dockx
