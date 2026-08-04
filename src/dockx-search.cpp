/*
DockX for OBS Studio (by StrmrX) -- project-wide source search.
GPL v2, see plugin-main.cpp for the full notice.

Walk every scene in the current collection (group children + nested scenes
included) and build one flat index of where each source lives, so a single
search box can find any source across the whole project and jump to it.
*/

#include "dockx.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

namespace dockx {
namespace search {

namespace {

/* friendly type label for a source ("Browser", "Image", "Scene", ...) */
static QString sourceTypeName(obs_source_t *src)
{
	if (!src)
		return QString();
	if (obs_source_get_type(src) == OBS_SOURCE_TYPE_SCENE)
		return QStringLiteral("Scene");
	const char *id = obs_source_get_id(src);
	const char *disp = id ? obs_source_get_display_name(id) : nullptr;
	if (disp && *disp)
		return QString::fromUtf8(disp);
	return id ? QString::fromUtf8(id) : QString();
}

struct ScanCtx {
	QList<Hit> *hits;
	QString sceneUuid;
	QString sceneName;
	QString groupName;  /* set while descending into a group */
	QSet<QString> *used; /* source names that appear in >= 1 scene */
};

static bool itemEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	ScanCtx *ctx = static_cast<ScanCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	const char *name = src ? obs_source_get_name(src) : nullptr;
	if (name && *name) {
		Hit h;
		h.sourceName = QString::fromUtf8(name);
		h.isGroup = obs_sceneitem_is_group(item);
		h.sourceType = h.isGroup ? QStringLiteral("Group")
					 : sourceTypeName(src);
		h.sceneUuid = ctx->sceneUuid;
		h.sceneName = ctx->sceneName;
		h.groupName = ctx->groupName;
		h.itemId = (long long)obs_sceneitem_get_id(item);
		h.visible = obs_sceneitem_visible(item);
		h.locked = obs_sceneitem_locked(item);
		ctx->hits->append(h);
		if (ctx->used)
			ctx->used->insert(h.sourceName);

		if (h.isGroup) {
			ScanCtx sub = *ctx;
			sub.groupName = h.sourceName;
			obs_sceneitem_group_enum_items(item, itemEnum, &sub);
		}
	}
	return true;
}

/* obs_enum_sources visits inputs (not scenes); anything not in the used set
   is loaded but placed in no scene */
static bool collectUnused(void *param, obs_source_t *src)
{
	ScanCtx *ctx = static_cast<ScanCtx *>(param);
	if (obs_source_get_output_flags(src) & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)) {
		const char *n = obs_source_get_name(src);
		if (n && *n && !ctx->used->contains(QString::fromUtf8(n))) {
			Hit h;
			h.sourceName = QString::fromUtf8(n);
			h.sourceType = sourceTypeName(src);
			/* sceneUuid empty + itemId 0 => unused */
			ctx->hits->append(h);
		}
	}
	return true;
}

/* deselect everything, then find one item by id (groups descended) */
static bool deselectEnum(obs_scene_t *, obs_sceneitem_t *item, void *)
{
	obs_sceneitem_select(item, false);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, deselectEnum, nullptr);
	return true;
}

struct FindCtx {
	long long id;
	obs_sceneitem_t *hit;
};

static bool findEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	FindCtx *c = static_cast<FindCtx *>(param);
	if ((long long)obs_sceneitem_get_id(item) == c->id) {
		c->hit = item;
		return false;
	}
	if (obs_sceneitem_is_group(item)) {
		obs_sceneitem_group_enum_items(item, findEnum, param);
		if (c->hit)
			return false;
	}
	return true;
}

} // namespace

QList<Hit> findAll()
{
	QList<Hit> hits;
	QSet<QString> used;

	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *s = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(s);
		if (!scene)
			continue;
		ScanCtx ctx;
		ctx.hits = &hits;
		ctx.used = &used;
		const char *u = obs_source_get_uuid(s);
		const char *n = obs_source_get_name(s);
		ctx.sceneUuid = QString::fromUtf8(u ? u : "");
		ctx.sceneName = QString::fromUtf8(n ? n : "");
		obs_scene_enum_items(scene, itemEnum, &ctx);
	}
	obs_frontend_source_list_free(&scenes);

	ScanCtx unusedCtx;
	unusedCtx.hits = &hits;
	unusedCtx.used = &used;
	obs_enum_sources(collectUnused, &unusedCtx);

	return hits;
}

bool reveal(const QString &sceneUuid, long long itemId)
{
	if (sceneUuid.isEmpty())
		return false;
	obs_source_t *sceneSrc =
		obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!sceneSrc)
		return false;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	bool ok = false;
	if (scene) {
		if (obs_frontend_preview_program_mode_active())
			obs_frontend_set_current_preview_scene(sceneSrc);
		else
			obs_frontend_set_current_scene(sceneSrc);

		obs_scene_enum_items(scene, deselectEnum, nullptr);
		FindCtx fc;
		fc.id = itemId;
		fc.hit = nullptr;
		obs_scene_enum_items(scene, findEnum, &fc);
		if (fc.hit) {
			obs_sceneitem_select(fc.hit, true);
			ok = true;
		}
	}
	obs_source_release(sceneSrc);
	return ok;
}

void openProperties(const QString &sourceName)
{
	obs_source_t *src =
		obs_get_source_by_name(sourceName.toUtf8().constData());
	if (!src)
		return;
	obs_frontend_open_source_properties(src);
	obs_source_release(src);
}

bool removeFromScene(const QString &sceneUuid, long long itemId)
{
	if (sceneUuid.isEmpty())
		return false;
	obs_source_t *sceneSrc =
		obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!sceneSrc)
		return false;
	obs_scene_t *scene = obs_scene_from_source(sceneSrc);
	bool ok = false;
	if (scene) {
		FindCtx fc;
		fc.id = itemId;
		fc.hit = nullptr;
		obs_scene_enum_items(scene, findEnum, &fc);
		if (fc.hit) {
			obs_sceneitem_remove(fc.hit);
			ok = true;
		}
	}
	obs_source_release(sceneSrc);
	return ok;
}

bool deleteSource(const QString &sourceName)
{
	obs_source_t *src =
		obs_get_source_by_name(sourceName.toUtf8().constData());
	if (!src)
		return true; /* already gone */
	obs_source_remove(src);
	obs_source_release(src);
	/* obs_source_destroy synchronously unlinks a source from the public list
	   the instant its last ref drops, so a name lookup now tells us for sure
	   whether it truly went away or something else still holds it */
	obs_source_t *check =
		obs_get_source_by_name(sourceName.toUtf8().constData());
	if (check) {
		obs_source_release(check);
		return false;
	}
	return true;
}

namespace {
struct FilterParentCtx {
	QString name;
	QString parent;
};
static bool findFilterParent(void *param, obs_source_t *src)
{
	auto *ctx = static_cast<FilterParentCtx *>(param);
	obs_source_t *f =
		obs_source_get_filter_by_name(src, ctx->name.toUtf8().constData());
	if (f) {
		ctx->parent = QString::fromUtf8(obs_source_get_name(src));
		obs_source_release(f);
		return false;
	}
	return true;
}
} // namespace

QString describeHolders(const QString &sourceName)
{
	QStringList reasons;

	/* our own live source docks (the one holder DockX itself can create) */
	for (const SourceDockEntry &e : state().sourceDocks)
		if (e.kind == sourcedocks::KIND_SOURCE &&
		    e.sourceName == sourceName) {
			reasons << "a DockX live dock is showing it (Tools > DockX > "
				   "Source docks, remove that dock)";
			break;
		}

	/* assigned as a global audio device in Settings > Audio */
	for (uint32_t ch = 0; ch < MAX_CHANNELS; ch++) {
		obs_source_t *s = obs_get_output_source(ch);
		if (!s)
			continue;
		const char *n = obs_source_get_name(s);
		if (n && QString::fromUtf8(n) == sourceName)
			reasons << "it is assigned as a global audio device (OBS "
				   "Settings > Audio, set it to Disabled)";
		obs_source_release(s);
	}

	/* used as a filter on another source */
	FilterParentCtx fc;
	fc.name = sourceName;
	obs_enum_sources(findFilterParent, &fc);
	if (!fc.parent.isEmpty())
		reasons << QString("it is a filter on \"%1\" (open that source's "
				   "Filters and remove it)")
				   .arg(fc.parent);

	return reasons.join("; ");
}

} // namespace search
} // namespace dockx
