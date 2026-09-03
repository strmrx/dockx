/*
DockX for OBS Studio (by StrmrX) -- cross-collection copy.
GPL v2, see plugin-main.cpp for the full notice.

Copy a scene (and every source it uses) from the current scene collection into
ANOTHER collection, without export files. OBS has no API to write a collection
that is not loaded, so we edit that collection's saved JSON on disk -- which is
safe precisely because OBS only reads a collection file when it switches to it.

Three hard safety rules, because this touches a user's saved setup:
  1. NEVER the active collection (OBS owns that file in memory and rewrites it).
  2. ADDITIVE ONLY -- we only append; no existing source or scene is changed or
     removed. A source whose name already exists in the target is left alone and
     the copied scene simply binds to it.
  3. A full .dockx-bak of the target file is written before we touch it.

If anything looks off at any step, do nothing and report it. Never crash OBS.
*/

#include "dockx.hpp"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <plugin-support.h>

#include <QDir>
#include <QFile>
#include <QSet>

#include <vector>

namespace dockx {
namespace collections {

QStringList otherCollections()
{
	QStringList out;
	char *cur = obs_frontend_get_current_scene_collection();
	const QString curName = cur ? QString::fromUtf8(cur) : QString();
	bfree(cur);
	char **names = obs_frontend_get_scene_collections();
	for (char **p = names; p && *p; p++) {
		const QString n = QString::fromUtf8(*p);
		if (!n.isEmpty() && n != curName)
			out << n;
	}
	bfree(names);
	return out;
}

/* the on-disk file whose "name" matches a collection (OBS slugs file names, so
   we identify by the name field inside, exactly as OBS itself does) */
static QString collectionFilePath(const QString &name)
{
	char *dir = os_get_config_path_ptr("obs-studio/basic/scenes");
	if (!dir)
		return QString();
	QDir d(QString::fromUtf8(dir));
	bfree(dir);
	const QStringList files = d.entryList(QStringList() << "*.json", QDir::Files);
	for (const QString &f : files) {
		const QString full = d.filePath(f);
		obs_data_t *cd = obs_data_create_from_json_file(full.toUtf8().constData());
		if (!cd)
			continue;
		const QString cn = QString::fromUtf8(obs_data_get_string(cd, "name"));
		obs_data_release(cd);
		if (cn == name)
			return full;
	}
	return QString();
}

/* gather every source a scene references, unique by name, recursing through
   groups and nested scenes so the copy is complete */
struct Collect {
	QSet<QString> names;
	std::vector<obs_source_t *> sources;
};

static bool collectCb(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	Collect *c = static_cast<Collect *>(param);
	obs_source_t *src = obs_sceneitem_get_source(item);
	if (src) {
		const QString n = QString::fromUtf8(obs_source_get_name(src));
		if (!c->names.contains(n)) {
			c->names.insert(n);
			c->sources.push_back(src);
			/* a nested scene (not a group) carries its own items */
			obs_scene_t *nested = obs_scene_from_source(src);
			if (nested && !obs_sceneitem_is_group(item))
				obs_scene_enum_items(nested, collectCb, param);
		}
	}
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, collectCb, param);
	return true;
}

/* strip the source uuid so OBS mints a fresh one on load: two collection files
   must never carry the same source uuid. Scene items bind by name, so this is
   safe. */
static void stripUuid(obs_data_t *d)
{
	obs_data_erase(d, "uuid");
}

static QString uniqueName(const QSet<QString> &taken, const QString &base)
{
	if (!taken.contains(base))
		return base;
	QString candidate = base + " (copy)";
	int n = 2;
	while (taken.contains(candidate))
		candidate = QString("%1 (copy %2)").arg(base).arg(n++);
	return candidate;
}

CopyReport copySceneToCollection(const QString &sceneUuid, const QString &target)
{
	CopyReport r;

	char *cur = obs_frontend_get_current_scene_collection();
	const QString curName = cur ? QString::fromUtf8(cur) : QString();
	bfree(cur);
	if (target == curName) {
		r.error = "That is the collection you are in now. Copy into a "
			  "different one (OBS manages the active collection itself).";
		return r;
	}

	obs_source_t *scene = obs_get_source_by_uuid(sceneUuid.toUtf8().constData());
	if (!scene) {
		r.error = "That scene could not be found.";
		return r;
	}
	obs_scene_t *sc = obs_scene_from_source(scene);
	const QString sceneName = QString::fromUtf8(obs_source_get_name(scene));
	if (!sc) {
		obs_source_release(scene);
		r.error = "That is not a scene.";
		return r;
	}

	const QString path = collectionFilePath(target);
	if (path.isEmpty()) {
		obs_source_release(scene);
		r.error = QString("Could not find \"%1\" saved on disk.").arg(target);
		return r;
	}

	obs_data_t *tdata = obs_data_create_from_json_file(path.toUtf8().constData());
	if (!tdata) {
		obs_source_release(scene);
		r.error = QString("Could not read \"%1\".").arg(target);
		return r;
	}

	obs_data_array_t *tsources = obs_data_get_array(tdata, "sources");
	if (!tsources) {
		tsources = obs_data_array_create();
		obs_data_set_array(tdata, "sources", tsources);
	}
	obs_data_array_t *torder = obs_data_get_array(tdata, "scene_order");
	if (!torder) {
		torder = obs_data_array_create();
		obs_data_set_array(tdata, "scene_order", torder);
	}

	/* names already present in the target -- we never overwrite these */
	QSet<QString> existing;
	const size_t tn = obs_data_array_count(tsources);
	for (size_t i = 0; i < tn; i++) {
		obs_data_t *e = obs_data_array_item(tsources, i);
		existing.insert(QString::fromUtf8(obs_data_get_string(e, "name")));
		obs_data_release(e);
	}

	/* collect + append the scene's sources (skip name collisions) */
	Collect c;
	obs_scene_enum_items(sc, collectCb, &c);
	for (obs_source_t *src : c.sources) {
		const QString n = QString::fromUtf8(obs_source_get_name(src));
		if (existing.contains(n)) {
			r.sourcesSkipped++;
			continue;
		}
		obs_data_t *sd = obs_save_source(src);
		if (!sd)
			continue;
		stripUuid(sd);
		obs_data_array_push_back(tsources, sd);
		obs_data_release(sd);
		existing.insert(n);
		r.sourcesCopied++;
	}

	/* append the scene itself, unique-named so it never clobbers one there */
	const QString finalName = uniqueName(existing, sceneName);
	obs_data_t *scd = obs_save_source(scene);
	stripUuid(scd);
	if (finalName != sceneName)
		obs_data_set_string(scd, "name", finalName.toUtf8().constData());
	obs_data_array_push_back(tsources, scd);
	obs_data_release(scd);

	obs_data_t *ord = obs_data_create();
	obs_data_set_string(ord, "name", finalName.toUtf8().constData());
	obs_data_array_push_back(torder, ord);
	obs_data_release(ord);

	obs_data_array_release(tsources);
	obs_data_array_release(torder);
	obs_source_release(scene);

	/* back up the target file, then write */
	const QString backup = path + ".dockx-bak";
	QFile::remove(backup);
	if (!QFile::copy(path, backup)) {
		obs_data_release(tdata);
		r.error = "Could not back up the target collection, so nothing was "
			  "written. Your files are untouched.";
		return r;
	}

	const bool ok = obs_data_save_json_safe(tdata, path.toUtf8().constData(), "tmp", "bak");
	obs_data_release(tdata);
	if (!ok) {
		r.error = "Writing the target collection failed. Its backup is at " + backup;
		return r;
	}

	r.ok = true;
	r.finalSceneName = finalName;
	r.backupPath = backup;
	obs_log(LOG_INFO, "copied scene \"%s\" into collection \"%s\" (%d sources, %d skipped)",
		finalName.toUtf8().constData(), target.toUtf8().constData(), r.sourcesCopied, r.sourcesSkipped);
	return r;
}

} // namespace collections
} // namespace dockx
