#include "grit_gdx_apply.h"

#include "grit_gdx_builder.h"
#include "grit_gdx_generator.h"
#include "grit_gdx_manifest.h"
#include "grit_gdx_job.h"

#include "modules/gdscript/gdscript_cache.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/resource.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "scene/gui/dialogs.h"
#include "scene/main/scene_tree.h"

#include "core/extension/gdextension_manager.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/string/string_builder.h"

// ---------------------------------------------------------------------------
// .tscn helpers
// ---------------------------------------------------------------------------

// Reads key="value" from a scene header line; the key must start an attribute so
// id=" never matches the id inside uid=".
static bool scene_attribute(const String &p_line, const String &p_key, String &r_value) {
	const String needle = p_key + "=\"";
	int position = p_line.find(needle);
	while (position >= 0) {
		const bool starts_attribute = position == 0 || p_line[position - 1] == ' ' || p_line[position - 1] == '\t';
		if (starts_attribute) {
			const int value_start = position + needle.length();
			const int value_end = p_line.find("\"", value_start);
			if (value_end <= value_start) {
				return false;
			}
			r_value = p_line.substr(value_start, value_end - value_start);
			return true;
		}
		position = p_line.find(needle, position + 1);
	}
	return false;
}

struct SceneNodeInfo {
	int line = -1;
};

static Vector<SceneNodeInfo> scene_nodes(const PackedStringArray &p_lines) {
	Vector<SceneNodeInfo> nodes;
	for (int i = 0; i < p_lines.size(); i++) {
		if (!p_lines[i].begins_with("[node ")) {
			continue;
		}
		SceneNodeInfo node;
		node.line = i;
		nodes.push_back(node);
	}
	return nodes;
}

// Finds the ExtResource id that points at `p_script_path` in this scene.
static String script_resource_id(const PackedStringArray &p_lines, const String &p_script_path) {
	for (const String &line : p_lines) {
		if (!line.begins_with("[ext_resource")) {
			continue;
		}
		String path;
		if (!scene_attribute(line, "path", path) || path != p_script_path) {
			continue;
		}
		String id;
		if (scene_attribute(line, "id", id)) {
			return id;
		}
	}
	return String();
}

static Error write_scene(const String &p_scene_path, const PackedStringArray &p_lines, const Vector<int> &p_dropped) {
	StringBuilder output;
	bool first = true;
	for (int i = 0; i < p_lines.size(); i++) {
		if (p_dropped.has(i)) {
			continue;
		}
		if (!first) {
			output.append("\n");
		}
		output.append(p_lines[i]);
		first = false;
	}
	Ref<FileAccess> file = FileAccess::open(p_scene_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_CREATE, vformat("Grit: could not write \"%s\".", p_scene_path));
	String text = output.as_string();
	if (!text.ends_with("\n")) {
		text += "\n";
	}
	file->store_string(text);
	return OK;
}

// ---------------------------------------------------------------------------
// project scanning
// ---------------------------------------------------------------------------

Vector<String> GritGdxApply::collect_scenes(const String &p_directory) {
	Vector<String> scenes;
	Ref<DirAccess> directory = DirAccess::open(p_directory);
	if (directory.is_null()) {
		return scenes;
	}
	for (const String &name : directory->get_files()) {
		if (name.get_extension() == "tscn") {
			scenes.push_back(p_directory.path_join(name));
		}
	}
	for (const String &name : directory->get_directories()) {
		if (name == ".godot" || name == "gdext") {
			continue;
		}
		const Vector<String> nested = collect_scenes(p_directory.path_join(name));
		scenes.append_array(nested);
	}
	return scenes;
}

Error GritGdxApply::build(const String &p_extension_directory, const String &p_target, const Progress &p_progress, String &r_library, String &r_error) {
	if (p_progress) {
		p_progress(10, "Preparing godot-cpp");
	}
	const String name = GritGdxManifest::extension_name(p_extension_directory);
	if (!GritGdxBuilder::build(p_extension_directory, name, p_target, r_library, r_error)) {
		return ERR_CANT_CREATE;
	}
	if (p_progress) {
		p_progress(90, "GDExtension library built");
	}
	if (!FileAccess::exists(r_library)) {
		r_error = vformat("the build reported success but \"%s\" is missing", r_library);
		return ERR_CANT_CREATE;
	}
	// The descriptor is published only now that the library exists, so the editor's
	// scan loads it once and without a missing-library error.
	const Error descriptor_error = GritGdxGenerator::write_extension_files(p_extension_directory);
	if (descriptor_error != OK) {
		r_error = "the library was built but the .gdextension descriptor could not be written";
		return descriptor_error;
	}
	return OK;
}

// ---------------------------------------------------------------------------
// source rewriting for migrated classes
// ---------------------------------------------------------------------------

// The value of a (now rejected) top-level `class_name X`.
static String class_name_of(const String &p_text) {
	for (const String &line : p_text.split("\n")) {
		const String stripped = line.strip_edges();
		if (stripped.begins_with("class_name ")) {
			return stripped.substr(11).strip_edges();
		}
	}
	return String();
}

static String strip_annotation_line(const String &p_text) {
	PackedStringArray lines = p_text.split("\n");
	Vector<int> dropped;
	for (int i = 0; i < lines.size(); i++) {
		if (lines[i].strip_edges().begins_with("@gdextension_class_name(")) {
			dropped.push_back(i);
			break;
		}
	}
	StringBuilder output;
	bool first = true;
	for (int i = 0; i < lines.size(); i++) {
		if (dropped.has(i)) {
			continue;
		}
		if (!first) {
			output.append("\n");
		}
		output.append(lines[i]);
		first = false;
	}
	return output.as_string();
}

static String strip_class_name_line(const String &p_text) {
	PackedStringArray lines = p_text.split("\n");
	Vector<int> dropped;
	for (int i = 0; i < lines.size(); i++) {
		if (lines[i].strip_edges().begins_with("class_name ")) {
			dropped.push_back(i);
			break;
		}
	}
	StringBuilder output;
	bool first = true;
	for (int i = 0; i < lines.size(); i++) {
		if (dropped.has(i)) {
			continue;
		}
		if (!first) {
			output.append("\n");
		}
		output.append(lines[i]);
		first = false;
	}
	return output.as_string();
}

// Inserts `class_name <name>` after leading comments/annotations and before the
// first statement, unless the source already declares one.
static String insert_class_name(const String &p_text, const String &p_class_name) {
	for (const String &line : p_text.split("\n")) {
		if (line.strip_edges().begins_with("class_name ")) {
			return p_text;
		}
	}
	PackedStringArray lines = p_text.split("\n");
	int insert_at = lines.size();
	for (int i = 0; i < lines.size(); i++) {
		const String stripped = lines[i].strip_edges();
		if (stripped.is_empty() || stripped.begins_with("#") || stripped.begins_with("@")) {
			continue;
		}
		insert_at = i;
		break;
	}
	lines.insert(insert_at, "class_name " + p_class_name);
	StringBuilder output;
	for (int i = 0; i < lines.size(); i++) {
		if (i > 0) {
			output.append("\n");
		}
		output.append(lines[i]);
	}
	return output.as_string();
}

static Error write_text_file(const String &p_path, const String &p_text) {
	DirAccess::make_dir_recursive_absolute(p_path.get_base_dir());
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_CREATE;
	}
	file->store_string(p_text);
	return OK;
}

String GritGdxApply::suggested_class_name(const String &p_script_path) {
	const String base = GritGdxGenerator::suggested_class_name(p_script_path);
	String candidate = base;
	int suffix = 2;
	while (ClassDB::class_exists(candidate) || ScriptServer::is_global_class(candidate)) {
		candidate = base + itos(suffix);
		suffix++;
	}
	return candidate;
}

// ---------------------------------------------------------------------------
// migration of classes recorded by the retired conversion flow
// ---------------------------------------------------------------------------

// Drops the `.gde` assignment from the nodes of the class (the legacy archive was
// attached to them). Returns how many nodes were detached.
static int detach_archive_assignments(const String &p_scene_path, const String &p_class_name, const String &p_archive_path, String &r_error) {
	Error read_error = OK;
	const String text = FileAccess::get_file_as_string(p_scene_path, &read_error);
	if (read_error != OK || !text.contains(p_archive_path)) {
		return 0;
	}
	PackedStringArray lines = text.split("\n");
	const String id = script_resource_id(lines, p_archive_path);
	if (id.is_empty()) {
		return 0;
	}
	const String assignment = "script = ExtResource(\"" + id + "\")";
	Vector<int> dropped;
	int detached = 0;
	for (const SceneNodeInfo &node : scene_nodes(lines)) {
		const String header = lines[node.line];
		const int type_position = header.find("type=\"");
		if (type_position < 0) {
			continue;
		}
		const int value_start = type_position + 6;
		const int value_end = header.find("\"", value_start);
		if (value_end < 0 || header.substr(value_start, value_end - value_start) != p_class_name) {
			continue;
		}
		for (int i = node.line + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
			if (lines[i].strip_edges() == assignment) {
				dropped.push_back(i);
				detached++;
				break;
			}
		}
	}
	if (detached == 0) {
		return 0;
	}
	// Drop the archive's ExtResource entry when nothing else references it.
	bool used = false;
	for (int i = 0; i < lines.size(); i++) {
		if (!dropped.has(i) && lines[i].strip_edges() == assignment) {
			used = true;
			break;
		}
	}
	if (!used) {
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].begins_with("[ext_resource") && lines[i].contains("path=\"" + p_archive_path + "\"")) {
				dropped.push_back(i);
				break;
			}
		}
	}
	if (write_scene(p_scene_path, lines, dropped) != OK) {
		r_error = vformat("could not rewrite \"%s\"", p_scene_path);
		return 0;
	}
	return detached;
}

Variant GritGdxApply::class_stale_badge(const StringName &p_class) {
	struct CacheEntry {
		uint64_t mtime = 0;
		String built_hash;
		bool stale = false;
	};
	static HashMap<StringName, CacheEntry> cache;

	for (const String &directory : GritGdxManifest::list_extensions()) {
		GritGdxManifest manifest;
		if (manifest.load(directory) != OK) {
			continue;
		}
		const GritGdxManifestEntry *entry = manifest.find_class(p_class);
		if (!entry) {
			continue;
		}
		if (entry->script_path.is_empty() || entry->source_hash.is_empty() || !FileAccess::exists(entry->script_path)) {
			return Variant();
		}
		const uint64_t mtime = FileAccess::get_modified_time(entry->script_path);
		CacheEntry *cached = cache.getptr(p_class);
		bool stale = false;
		if (cached && cached->mtime == mtime && cached->built_hash == entry->source_hash) {
			stale = cached->stale;
		} else {
			Error error = OK;
			const String source = FileAccess::get_file_as_string(entry->script_path, &error);
			stale = error == OK && source.sha256_text() != entry->source_hash;
			CacheEntry value;
			value.mtime = mtime;
			value.built_hash = entry->source_hash;
			value.stale = stale;
			cache[p_class] = value;
		}
		if (!stale) {
			return Variant();
		}
		Dictionary badge;
		badge["icon"] = StringName("GritGdxStaleBadge");
		badge["tooltip"] = String("The .gde source of this generated class changed since the extension was built. Regenerate and rebuild it (--grit-gdx-generate).");
		return badge;
	}
	return Variant();
}

Error GritGdxApply::migrate_legacy(Vector<String> &r_migrated, String &r_report) {
	r_migrated.clear();
	StringBuilder report;
	for (const String &directory : GritGdxManifest::list_extensions()) {
		GritGdxManifest manifest;
		if (manifest.load(directory) != OK) {
			report.append(vformat("Grit: could not read \"%s\"; skipped.\n", directory));
			continue;
		}
		bool changed = false;
		for (GritGdxManifestEntry &entry : manifest.classes) {
			const bool source_exists = !entry.script_path.is_empty() && FileAccess::exists(entry.script_path);
			const bool archive_exists = !entry.archived_script.is_empty() && FileAccess::exists(entry.archived_script);
			if (!source_exists && !archive_exists) {
				continue;
			}
			if (entry.script_path.ends_with(".gde") && entry.archived_script.is_empty() && source_exists) {
				continue; // already a GDExtension source
			}
			const String old_source = source_exists ? entry.script_path : entry.archived_script;
			Error read_error = OK;
			const String text = FileAccess::get_file_as_string(old_source, &read_error);
			if (read_error != OK) {
				report.append(vformat("Grit: could not read the source of \"%s\"; skipped.\n", entry.class_name));
				continue;
			}
			// One model: a `.gde` source that declares `class_name <name>`.
			String name = !entry.class_name.is_empty() ? entry.class_name : class_name_of(text);
			if (name.is_empty()) {
				name = GritGdxApply::suggested_class_name(old_source);
			}
			String rewritten = strip_class_name_line(text);
			rewritten = strip_annotation_line(rewritten);
			rewritten = insert_class_name(rewritten, name);
			const String new_path = old_source.get_basename() + ".gde";
			if (write_text_file(new_path, rewritten) != OK) {
				report.append(vformat("Grit: could not write \"%s\"; skipped \"%s\".\n", new_path, entry.class_name));
				continue;
			}
			if (old_source != new_path) {
				if (FileAccess::exists(old_source + ".uid")) {
					if (FileAccess::exists(new_path + ".uid")) {
						DirAccess::remove_absolute(new_path + ".uid");
					}
					DirAccess::rename_absolute(old_source + ".uid", new_path + ".uid");
				}
				DirAccess::remove_absolute(old_source);
			}
			// Drop a legacy archive (and detach the nodes it was attached to).
			if (!entry.archived_script.is_empty() && entry.archived_script != new_path && archive_exists) {
				for (const String &scene : collect_scenes("res://")) {
					String detach_error;
					detach_archive_assignments(scene, entry.class_name, entry.archived_script, detach_error);
					if (!detach_error.is_empty()) {
						report.append("Grit: " + detach_error + "\n");
					}
				}
				DirAccess::remove_absolute(entry.archived_script);
				DirAccess::remove_absolute(entry.archived_script + ".uid");
			}
			entry.script_path = new_path;
			entry.archived_script = String();
			Error hash_error = OK;
			const String source = FileAccess::get_file_as_string(new_path, &hash_error);
			entry.source_hash = hash_error == OK ? source.sha256_text() : String();
			changed = true;
			r_migrated.push_back(entry.class_name);
			report.append(vformat("Grit: upgraded \"%s\": source %s.\n", entry.class_name, new_path));
		}
		if (changed) {
			const Error save_error = manifest.save(directory);
			if (save_error != OK) {
				report.append(vformat("Grit: could not write the manifest in \"%s\".\n", directory));
				continue;
			}
			GritGdxGenerator::write_extension_files(directory);
		}
	}
	if (r_migrated.is_empty()) {
		report.append("Grit: nothing to upgrade (every class already uses a .gde source).\n");
	}
	r_report = report.as_string();
	return OK;
}

static Ref<GritGdxJob> build_source_job;
static AcceptDialog *build_source_dialog = nullptr;

static void _schedule_build_source_poll() {
	if (SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop())) {
		tree->create_timer(0.25)->connect(SNAME("timeout"), callable_mp_static(&GritGdxApply::poll_build_source));
	}
}

void GritGdxApply::build_source_from_editor(const String &p_script_path) {
	if (build_source_job.is_valid()) {
		EditorNode::get_singleton()->show_accept("A GDExtension build is already running.", "Build GDE");
		return;
	}
	if (!p_script_path.ends_with(".gde")) {
		EditorNode::get_singleton()->show_accept("Open a GDExtension source (.gde) first.", "Build GDE");
		return;
	}
	build_source_job.instantiate();
	const Error start_error = build_source_job->start_build_source(p_script_path);
	if (start_error != OK) {
		String report;
		build_source_job->complete(report);
		build_source_job.unref();
		EditorNode::get_singleton()->show_accept(report, "Build GDE");
		return;
	}
	if (build_source_dialog == nullptr) {
		build_source_dialog = memnew(AcceptDialog);
		build_source_dialog->set_title("Build GDE");
		build_source_dialog->set_min_size(Size2i(560, 180));
		EditorNode::get_singleton()->get_gui_base()->add_child(build_source_dialog);
	}
	build_source_dialog->set_text(vformat("Building %s...", p_script_path.get_file()));
	build_source_dialog->popup_centered();
	_schedule_build_source_poll();
}

void GritGdxApply::poll_build_source() {
	if (build_source_job.is_null()) {
		return;
	}
	if (!build_source_job->is_finished()) {
		if (build_source_dialog) {
			build_source_dialog->set_text(vformat("Building... %d%%\n%s", build_source_job->get_progress(), build_source_job->get_status()));
		}
		_schedule_build_source_poll();
		return;
	}
	String report;
	const Error error = build_source_job->complete(report);
	build_source_job.unref();
	if (build_source_dialog) {
		build_source_dialog->set_text(report);
		build_source_dialog->popup_centered();
	}
	if (error != OK) {
		ERR_PRINT(report);
	} else {
		print_line(report);
	}
	// The descriptor was written after the build; make the editor import it and
	// register its classes (the extension list the game reads is written here too).
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	if (GDExtensionManager::get_singleton()) {
		GDExtensionManager::get_singleton()->reload_extensions();
	}
	if (!OS::get_singleton()->get_environment("GRIT_GDX_BUILD_TEST").is_empty()) {
		print_line("Grit: build test finished.");
		if (SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop())) {
			tree->quit();
		}
	}
}

static void _collect_gde_sources(const String &p_directory, Vector<String> &r_sources) {
	// Generated extension folders own a manifest; never descend into them.
	if (FileAccess::exists(p_directory.path_join(".grit_manifest.json"))) {
		return;
	}
	for (const String &file : DirAccess::get_files_at(p_directory)) {
		if (file.get_extension().to_lower() == "gde") {
			r_sources.push_back(p_directory.path_join(file));
		}
	}
	for (const String &directory : DirAccess::get_directories_at(p_directory)) {
		if (directory.begins_with(".")) {
			continue;
		}
		_collect_gde_sources(p_directory.path_join(directory), r_sources);
	}
}

static void _collect_manifest_dirs(const String &p_directory, Vector<String> &r_dirs, const String &p_skip) {
	if (FileAccess::exists(p_directory.path_join(".grit_manifest.json")) && p_directory != p_skip) {
		r_dirs.push_back(p_directory);
	}
	for (const String &directory : DirAccess::get_directories_at(p_directory)) {
		if (directory.begins_with(".") || directory == "src" || directory == "bin") {
			continue;
		}
		_collect_manifest_dirs(p_directory.path_join(directory), r_dirs, p_skip);
	}
}

static void _remove_directory(const String &p_directory) {
	Ref<DirAccess> directory = DirAccess::open(p_directory);
	if (directory.is_null()) {
		return;
	}
	directory->set_include_hidden(true);
	directory->list_dir_begin();
	for (String name = directory->get_next(); !name.is_empty(); name = directory->get_next()) {
		if (name == "." || name == "..") {
			continue;
		}
		if (directory->current_is_dir()) {
			_remove_directory(p_directory.path_join(name));
		} else {
			DirAccess::remove_absolute(p_directory.path_join(name));
		}
	}
	directory->list_dir_end();
	DirAccess::remove_absolute(p_directory);
}

Error GritGdxApply::refresh_project_extension(String &r_report) {
	Vector<String> sources;
	_collect_gde_sources("res://", sources);
	sources.sort();
	const String directory = project_extension_directory();

	// Classes whose .gde source is gone leave the project extension.
	GritGdxManifest manifest;
	if (manifest.load(directory) == OK) {
		bool changed = false;
		for (int i = manifest.classes.size() - 1; i >= 0; i--) {
			const String script = manifest.classes[i].script_path;
			if (script.ends_with(".gde") && !sources.has(script)) {
				manifest.classes.remove_at(i);
				changed = true;
			}
		}
		if (changed) {
			manifest.save(directory);
		}
	}

	StringBuilder report;
	int failures = 0;
	for (const String &source : sources) {
		String one;
		const Error error = create_source(source, one);
		report.append(one);
		if (error != OK) {
			failures++;
		}
	}
	GritGdxGenerator::write_extension_files(directory);
	r_report = report.as_string();
	if (sources.is_empty()) {
		r_report = "Grit: no .gde sources found in the project.\n" + r_report;
		return ERR_DOES_NOT_EXIST;
	}
	if (failures == sources.size()) {
		return ERR_INVALID_PARAMETER;
	}
	return OK;
}

void GritGdxApply::consolidate_project_extension(String &r_report) {
	StringBuilder report;
	const String project = project_extension_directory();
	GritGdxManifest project_manifest;
	project_manifest.load(project);
	Vector<String> dirs;
	_collect_manifest_dirs("res://", dirs, project);
	for (const String &dir : dirs) {
		GritGdxManifest old;
		if (old.load(dir) != OK) {
			continue;
		}
		bool subsumed = true;
		for (const GritGdxManifestEntry &entry : old.classes) {
			if (!entry.script_path.ends_with(".gde")) {
				subsumed = false;
				break;
			}
			const GritGdxManifestEntry *match = project_manifest.find_class(entry.class_name);
			if (!match || match->script_path != entry.script_path) {
				subsumed = false;
				break;
			}
		}
		if (!subsumed) {
			report.append(vformat("Grit: left \"%s\": it has classes that are not in the project extension.\n", dir));
			continue;
		}
		_remove_directory(dir);
		report.append(vformat("Grit: removed the old extension \"%s\" (its classes now live in \"%s\").\n", dir, project));
	}
	// A project rename leaves the old descriptor behind.
	for (const String &file : DirAccess::get_files_at(project)) {
		if (file.get_extension().to_lower() == "gdextension" && file != GritGdxManifest::extension_name(project) + ".gdextension") {
			DirAccess::remove_absolute(project.path_join(file));
			report.append(vformat("Grit: removed the stale descriptor \"%s\".\n", file));
		}
	}
	// The editor's extension list caches the descriptors; drop the removed ones so
	// the next launch (and a headless run) does not try to load them.
	const String extension_list = "res://.godot/extension_list.cfg";
	if (FileAccess::exists(extension_list)) {
		Error read_error = OK;
		const String list_text = FileAccess::get_file_as_string(extension_list, &read_error);
		if (read_error == OK) {
			StringBuilder kept;
			bool changed = false;
			for (const String &line : list_text.split("\n")) {
				const String path = line.strip_edges();
				if (!path.is_empty() && !FileAccess::exists(path)) {
					changed = true;
					continue;
				}
				kept.append(line);
				kept.append("\n");
			}
			if (changed) {
				write_text_file(extension_list, kept.as_string());
				report.append("Grit: updated the editor's extension list.\n");
			}
		}
	}
	r_report = report.as_string();
}

Error GritGdxApply::create_source(const String &p_script_path, String &r_report, String *r_extension_directory) {
	if (p_script_path.is_empty()) {
		r_report = "Grit: no script path given.";
		return ERR_INVALID_PARAMETER;
	}
	const String path = p_script_path.begins_with("res://") ? p_script_path : ProjectSettings::get_singleton()->localize_path(p_script_path);
	if (!FileAccess::exists(path)) {
		r_report = vformat("Grit: \"%s\" does not exist.", path);
		return ERR_FILE_NOT_FOUND;
	}
	Error read_error = OK;
	const String source = FileAccess::get_file_as_string(path, &read_error);
	if (read_error != OK) {
		r_report = vformat("Grit: could not read \"%s\".", path);
		return read_error;
	}
	const String script_class = class_name_of(source);
	if (script_class.is_empty()) {
		r_report = vformat("Grit: \"%s\" has no class_name; the generated class always takes the script's class_name.", path);
		return ERR_INVALID_PARAMETER;
	}
	const String directory = project_extension_directory();

	// Compile a temporary copy: the build never loads, compiles or otherwise
	// touches the `.gde` file itself.
	const String build_copy = vformat("user://grit_build_%d.gde", (int)OS::get_singleton()->get_ticks_usec());
	if (write_text_file(build_copy, source) != OK) {
		r_report = "Grit: could not create the temporary build copy.";
		return ERR_CANT_CREATE;
	}
	GritGdxClass klass;
	String report;
	const Error error = GritGdxGenerator::generate_class(directory, build_copy, klass, report, path);
	GDScriptCache::remove_script(build_copy);
	if (const Ref<Resource> cached = ResourceCache::get_ref(build_copy); cached.is_valid()) {
		cached->set_path(String());
	}
	DirAccess::remove_absolute(build_copy);
	DirAccess::remove_absolute(build_copy + ".uid");
	if (error != OK) {
		r_report = report;
		return error;
	}
	if (r_extension_directory) {
		*r_extension_directory = directory;
	}

	GritGdxManifest manifest;
	if (manifest.load(directory) == OK) {
		if (const GritGdxManifestEntry *entry = manifest.find_class(klass.class_name)) {
			GritGdxManifestEntry updated = *entry;
			updated.script_path = path;
			updated.archived_script = String();
			updated.source_hash = source.sha256_text();
			manifest.upsert(updated);
			manifest.save(directory);
		}
	}
	r_report = report + vformat("\n  source: %s\n  extension: %s\n", path, directory);
	return OK;
}

void GritGdxApply::create_source_callback(const String &p_script_path) {
	// A new .gde joins the project extension and builds it for the first time.
	build_source_from_editor(p_script_path);
}
