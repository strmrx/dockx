/*
DockX for OBS Studio (by StrmrX) -- source tags + bulk operations.
GPL v2, see plugin-main.cpp for the full notice.

User labels on sources (kept by source UUID so they survive a rename), plus
project-wide bulk actions: show/hide/lock/unlock every scene item of a set of
sources across every scene, and mute/unmute the ones that have audio.
*/

#include "dockx.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <QSet>

#include <algorithm>

namespace dockx {
namespace tags {

namespace {

/* friendly type label for a source ("Browser", "Image", ...) */
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

/* trim, drop empties, de-dupe (case-insensitive), sort */
static QStringList normalize(const QStringList &in)
{
	QStringList out;
	for (QString t : in) {
		t = t.trimmed();
		t.replace('\n', ' ');
		if (t.isEmpty())
			continue;
		bool dup = false;
		for (const QString &x : out)
			if (x.compare(t, Qt::CaseInsensitive) == 0) {
				dup = true;
				break;
			}
		if (!dup)
			out << t;
	}
	out.sort(Qt::CaseInsensitive);
	return out;
}

/* obs_enum_sources visits inputs (not scenes/groups/transitions); keep the ones
   that carry real video or audio, exactly like the project-wide search does */
static bool collectSources(void *param, obs_source_t *src)
{
	auto *list = static_cast<QList<SourceInfo> *>(param);
	const uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & (OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO)))
		return true;
	const char *n = obs_source_get_name(src);
	const char *u = obs_source_get_uuid(src);
	if (!n || !*n || !u || !*u)
		return true;
	SourceInfo si;
	si.uuid = QString::fromUtf8(u);
	si.name = QString::fromUtf8(n);
	si.type = sourceTypeName(src);
	si.hasAudio = (flags & OBS_SOURCE_AUDIO) != 0;
	si.tags = state().sourceTags.value(si.uuid);
	list->append(si);
	return true;
}

/* bulk visibility/lock: toggle every item whose source uuid is in the set
   (groups descended) */
struct BulkCtx {
	const QSet<QString> *uuids;
	Op op;
	int *affected;
};

static bool bulkItemEnum(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto *ctx = static_cast<BulkCtx *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src) {
		const char *u = obs_source_get_uuid(src);
		if (u && ctx->uuids->contains(QString::fromUtf8(u))) {
			switch (ctx->op) {
			case SHOW:
				obs_sceneitem_set_visible(item, true);
				break;
			case HIDE:
				obs_sceneitem_set_visible(item, false);
				break;
			case LOCK:
				obs_sceneitem_set_locked(item, true);
				break;
			case UNLOCK:
				obs_sceneitem_set_locked(item, false);
				break;
			default:
				break;
			}
			(*ctx->affected)++;
		}
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, bulkItemEnum, param);
	return true;
}

} // namespace

QList<SourceInfo> listSources()
{
	QList<SourceInfo> out;
	obs_enum_sources(collectSources, &out);
	std::sort(out.begin(), out.end(), [](const SourceInfo &a, const SourceInfo &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return out;
}

QStringList allTags()
{
	QStringList out;
	for (const SourceInfo &si : listSources())
		for (const QString &t : si.tags) {
			bool dup = false;
			for (const QString &x : out)
				if (x.compare(t, Qt::CaseInsensitive) == 0) {
					dup = true;
					break;
				}
			if (!dup)
				out << t;
		}
	out.sort(Qt::CaseInsensitive);
	return out;
}

QStringList tagsForSource(const QString &uuid)
{
	return state().sourceTags.value(uuid);
}

void addTagToSources(const QStringList &uuids, const QString &tag)
{
	const QString t = tag.trimmed();
	if (t.isEmpty())
		return;
	for (const QString &u : uuids) {
		if (u.isEmpty())
			continue;
		QStringList cur = state().sourceTags.value(u);
		cur << t;
		state().sourceTags[u] = normalize(cur);
	}
	stateSave();
}

void removeTagFromSources(const QStringList &uuids, const QString &tag)
{
	const QString t = tag.trimmed();
	if (t.isEmpty())
		return;
	for (const QString &u : uuids) {
		if (!state().sourceTags.contains(u))
			continue;
		QStringList kept;
		for (const QString &x : state().sourceTags.value(u))
			if (x.compare(t, Qt::CaseInsensitive) != 0)
				kept << x;
		if (kept.isEmpty())
			state().sourceTags.remove(u);
		else
			state().sourceTags[u] = kept;
	}
	stateSave();
}

BulkResult applyBulk(const QStringList &uuids, Op op)
{
	BulkResult r;
	if (uuids.isEmpty())
		return r;
	QSet<QString> set(uuids.begin(), uuids.end());

	if (op == MUTE || op == UNMUTE) {
		for (const QString &u : set) {
			obs_source_t *src = obs_get_source_by_uuid(u.toUtf8().constData());
			if (!src)
				continue;
			if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
				obs_source_set_muted(src, op == MUTE);
				r.affected++;
			}
			obs_source_release(src);
		}
		return r;
	}

	QSet<QString> touchedScenes;
	obs_frontend_source_list scenes = {};
	obs_frontend_get_scenes(&scenes);
	for (size_t i = 0; i < scenes.sources.num; i++) {
		obs_source_t *s = scenes.sources.array[i];
		obs_scene_t *scene = obs_scene_from_source(s);
		if (!scene)
			continue;
		const int before = r.affected;
		BulkCtx ctx{&set, op, &r.affected};
		obs_scene_enum_items(scene, bulkItemEnum, &ctx);
		if (r.affected > before) {
			const char *su = obs_source_get_uuid(s);
			if (su)
				touchedScenes.insert(QString::fromUtf8(su));
		}
	}
	obs_frontend_source_list_free(&scenes);
	r.scenes = (int)touchedScenes.size();
	return r;
}

} // namespace tags
} // namespace dockx
