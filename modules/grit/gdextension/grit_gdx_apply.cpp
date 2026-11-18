#include "grit_gdx_apply.h"

#include "grit_gdx_builder.h"
#include "grit_gdx_emitter.h"
#include "grit_gdx_generator.h"
#include "grit_gdx_manifest.h"
#include "grit_gdx_platform.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_cache.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/os/thread.h"

#include "editor/script/script_editor_plugin.h"
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
	String name;
	String parent;
};

static Vector<SceneNodeInfo> scene_nodes(const PackedStringArray &p_lines) {
	Vector<SceneNodeInfo> nodes;
	for (int i = 0; i < p_lines.size(); i++) {
		if (!p_lines[i].begins_with("[node ")) {
			continue;
		}
		SceneNodeInfo node;
		node.line = i;
		scene_attribute(p_lines[i], "name", node.name);
		scene_attribute(p_lines[i], "parent", node.parent);
		nodes.push_back(node);
	}
	return nodes;
}

// Path relative to the scene root: "." for the root, "A", "A/B", ...
static String node_scene_path(const SceneNodeInfo &p_node) {
	if (p_node.parent.is_empty()) {
		return ".";
	}
	if (p_node.parent == ".") {
		return p_node.name;
	}
	return p_node.parent + "/" + p_node.name;
}

static int find_node_line(const Vector<SceneNodeInfo> &p_nodes, const String &p_node_path) {
	for (const SceneNodeInfo &node : p_nodes) {
		if (node_scene_path(node) == p_node_path) {
			return node.line;
		}
	}
	return -1;
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

// Points an ext_resource line at another path, dropping its `uid`: the archive has
// no UID of its own, and a stale one would make the loader resolve the old file.
static String retarget_resource_line(const String &p_line, const String &p_path) {
	String line = p_line;
	const int uid_position = line.find(" uid=\"");
	if (uid_position >= 0) {
		const int uid_end = line.find("\"", uid_position + 6);
		if (uid_end >= 0) {
			line = line.substr(0, uid_position) + line.substr(uid_end + 1);
		}
	}
	const int path_position = line.find("path=\"");
	if (path_position >= 0) {
		const int path_end = line.find("\"", path_position + 6);
		if (path_end >= 0) {
			line = line.substr(0, path_position + 6) + p_path + line.substr(path_end);
		}
	}
	return line;
}

// How many nodes still assign their script through this ExtResource id.
static int script_assignment_users(const PackedStringArray &p_lines, const String &p_id) {
	const String assignment = "script = ExtResource(\"" + p_id + "\")";
	int users = 0;
	for (const String &line : p_lines) {
		if (line.strip_edges() == assignment) {
			users++;
		}
	}
	return users;
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

// A file that is about to be renamed must not stay open in the script editor: the
// editor would reload it mid-rename and report a missing script. The script
// editor is a control tree, so this only works on the main thread; the revert
// runs on a worker, which is why its caller closes the tabs before starting it.
void GritGdxApply::close_editor_script_tabs(const String &p_path) {
#ifdef TOOLS_ENABLED
	// The script editor is a control tree: calling into it from a worker thread
	// makes `get_tab_control()` fail at runtime. Revert runs on a worker, so its
	// caller does this before starting it; the guard makes a regression loud
	// instead of an error spam from inside the editor.
	if (!Thread::is_main_thread()) {
		ERR_PRINT("Grit: close_editor_script_tabs() must be called from the main thread.");
		return;
	}
	if (ScriptEditor *editor = ScriptEditor::get_singleton()) {
		editor->close_file(p_path);
	}
#endif
}

// Replaces the node's type and drops its script assignment: the class is the
// implementation, and the script is archived as a standalone `.gds` source, so a
// new `.gd` can take the node later without a second copy of the old one.
static Error convert_node(const String &p_scene_path, const String &p_node_path, const String &p_script_path, const String &p_class_name, bool &r_rewritten, String &r_error) {
	r_rewritten = false;
	Error error = OK;
	const String text = FileAccess::get_file_as_string(p_scene_path, &error);
	if (error != OK || !text.contains(p_script_path)) {
		return OK;
	}
	PackedStringArray lines = text.split("\n");
	const String id = script_resource_id(lines, p_script_path);
	if (id.is_empty()) {
		return OK;
	}
	const String script_line = "script = ExtResource(\"" + id + "\")";
	const Vector<SceneNodeInfo> nodes = scene_nodes(lines);

	Vector<int> targets;
	if (p_node_path.is_empty()) {
		// Every node that uses the script.
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].strip_edges() != script_line) {
				continue;
			}
			int header = i - 1;
			while (header >= 0 && !lines[header].begins_with("[node ")) {
				header--;
			}
			if (header >= 0) {
				targets.push_back(header);
			}
		}
	} else {
		const int header = find_node_line(nodes, p_node_path);
		if (header < 0) {
			r_error = vformat("node \"%s\" was not found in \"%s\"", p_node_path, p_scene_path);
			return ERR_DOES_NOT_EXIST;
		}
		bool has_script = false;
		for (int i = header + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
			if (lines[i].strip_edges() == script_line) {
				has_script = true;
				break;
			}
		}
		if (!has_script) {
			r_error = vformat("node \"%s\" in \"%s\" does not use \"%s\"", p_node_path, p_scene_path, p_script_path);
			return ERR_INVALID_PARAMETER;
		}
		targets.push_back(header);
	}
	if (targets.is_empty()) {
		return OK;
	}

	Vector<int> dropped;
	for (int header : targets) {
		String line = lines[header];
		const int type_position = line.find("type=\"");
		if (type_position < 0) {
			continue;
		}
		const int value_start = type_position + 6;
		const int value_end = line.find("\"", value_start);
		if (value_end < 0) {
			continue;
		}
		const String current_type = line.substr(value_start, value_end - value_start);
		if (ClassDB::class_exists(current_type)) {
			const ClassDB::APIType api = ClassDB::get_api_type(current_type);
			if (api == ClassDB::API_EXTENSION || api == ClassDB::API_EDITOR_EXTENSION) {
				continue;
			}
		}
		lines.write[header] = line.substr(0, value_start) + p_class_name + line.substr(value_end);
		for (int i = header + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
			if (lines[i].strip_edges() == script_line) {
				dropped.push_back(i);
				break;
			}
		}
		r_rewritten = true;
	}
	if (!r_rewritten) {
		return OK;
	}

	// Drop the script's ExtResource entry when nothing else uses it.
	bool still_used = false;
	for (int i = 0; i < lines.size(); i++) {
		if (!dropped.has(i) && lines[i].strip_edges() == script_line) {
			still_used = true;
			break;
		}
	}
	if (!still_used) {
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].begins_with("[ext_resource") && lines[i].contains("path=\"" + p_script_path + "\"")) {
				dropped.push_back(i);
				break;
			}
		}
	}
	return write_scene(p_scene_path, lines, dropped);
}

// Puts the node back to its script: base type plus a script assignment to the
// live `.gd`. The converted node currently points at the `.gde` archive, so the
// archive entry is reused when this node is its only user and a fresh entry is
// added otherwise.
static Error restore_node(const String &p_scene_path, const String &p_node_path, const String &p_script_path, const String &p_base_type, String &r_error) {
	Error error = OK;
	const String text = FileAccess::get_file_as_string(p_scene_path, &error);
	if (error != OK) {
		r_error = vformat("could not read \"%s\"", p_scene_path);
		return error;
	}
	PackedStringArray lines = text.split("\n");
	// New conversions archive as `.gds`; `.gde` is the legacy attached archive.
	String archived_path = p_script_path.get_basename() + ".gds";
	String archive_id = script_resource_id(lines, archived_path);
	if (archive_id.is_empty()) {
		archived_path = p_script_path.get_basename() + ".gde";
		archive_id = script_resource_id(lines, archived_path);
	}

	// An entry for the live script may still exist (a shared script keeps it), in
	// which case it is reused. Otherwise the archive's entry is retargeted when no
	// other converted node depends on it, and a new entry is created if it is.
	String id = script_resource_id(lines, p_script_path);
	if (id.is_empty()) {
		if (!archive_id.is_empty() && script_assignment_users(lines, archive_id) == 1) {
			for (int i = 0; i < lines.size(); i++) {
				if (lines[i].begins_with("[ext_resource") && lines[i].contains("id=\"" + archive_id + "\"") && lines[i].contains("path=\"" + archived_path + "\"")) {
					lines.write[i] = retarget_resource_line(lines[i], p_script_path);
					break;
				}
			}
			id = archive_id;
		}
	}
	if (id.is_empty()) {
		int suffix = 1;
		do {
			id = "grit_script" + (suffix == 1 ? String() : itos(suffix));
			suffix++;
		} while (text.contains("id=\"" + id + "\""));
		int insert_at = -1;
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].begins_with("[ext_resource")) {
				insert_at = i + 1;
			}
		}
		if (insert_at < 0) {
			for (int i = 0; i < lines.size(); i++) {
				if (lines[i].begins_with("[gd_scene")) {
					insert_at = i + 1;
					break;
				}
			}
		}
		// Keep the blank line the editor writes under `[gd_scene]`: insert the new
		// resource after it, not between the header and the gap.
		while (insert_at < lines.size() && lines[insert_at].strip_edges().is_empty()) {
			insert_at++;
		}
		lines.insert(insert_at, "[ext_resource type=\"Script\" path=\"" + p_script_path + "\" id=\"" + id + "\"]");
	}

	const Vector<SceneNodeInfo> nodes = scene_nodes(lines);
	const int header = find_node_line(nodes, p_node_path);
	if (header < 0) {
		r_error = vformat("node \"%s\" was not found in \"%s\"", p_node_path, p_scene_path);
		return ERR_DOES_NOT_EXIST;
	}

	// Restore the original type.
	String line = lines[header];
	const int type_position = line.find("type=\"");
	if (type_position >= 0) {
		const int value_start = type_position + 6;
		const int value_end = line.find("\"", value_start);
		lines.write[header] = line.substr(0, value_start) + p_base_type + line.substr(value_end);
	} else {
		const int insert = line.find("]");
		lines.write[header] = line.substr(0, insert) + " type=\"" + p_base_type + "\"" + line.substr(insert);
	}

	// Point the node at the live script, replacing the archive assignment.
	const String assignment = "script = ExtResource(\"" + id + "\")";
	int assignment_line = -1;
	for (int i = header + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
		if (lines[i].strip_edges().begins_with("script = ExtResource(\"")) {
			assignment_line = i;
			break;
		}
	}
	if (assignment_line >= 0) {
		lines.write[assignment_line] = assignment;
	} else {
		int insert_at = header + 1;
		while (insert_at < lines.size() && !lines[insert_at].begins_with("[node ") && !lines[insert_at].begins_with("[connection")) {
			insert_at++;
		}
		lines.insert(insert_at, assignment);
	}

	// The archive is renamed back to `.gd`, so an entry that nothing references
	// anymore would point at a file that no longer exists.
	Vector<int> dropped;
	for (int i = 0; i < lines.size(); i++) {
		if (!lines[i].begins_with("[ext_resource") || !lines[i].contains("path=\"" + archived_path + "\"")) {
			continue;
		}
		String entry_id;
		if (!scene_attribute(lines[i], "id", entry_id)) {
			continue;
		}
		if (script_assignment_users(lines, entry_id) == 0) {
			dropped.push_back(i);
		}
	}
	return write_scene(p_scene_path, lines, dropped);
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

int GritGdxApply::count_script_users(const String &p_script_path, Vector<String> *r_scenes) {
	int users = 0;
	for (const String &scene_path : collect_scenes("res://")) {
		Error error = OK;
		const String text = FileAccess::get_file_as_string(scene_path, &error);
		if (error != OK || !text.contains(p_script_path)) {
			continue;
		}
		const PackedStringArray lines = text.split("\n");
		const String id = script_resource_id(lines, p_script_path);
		if (id.is_empty()) {
			continue;
		}
		const String script_line = "script = ExtResource(\"" + id + "\")";
		int found = 0;
		for (const String &line : lines) {
			if (line.strip_edges() == script_line) {
				found++;
			}
		}
		if (found > 0) {
			users += found;
			if (r_scenes) {
				r_scenes->push_back(scene_path);
			}
		}
	}
	return users;
}

String GritGdxApply::scene_node_type(const String &p_scene_path, const String &p_node_path) {
	Error error = OK;
	const String text = FileAccess::get_file_as_string(p_scene_path, &error);
	if (error != OK) {
		return String();
	}
	const PackedStringArray lines = text.split("\n");
	const Vector<SceneNodeInfo> nodes = scene_nodes(lines);
	const int header = find_node_line(nodes, p_node_path);
	if (header < 0) {
		return String();
	}
	String type;
	scene_attribute(lines[header], "type", type);
	return type;
}

Vector<String> GritGdxApply::class_users(const String &p_class_name) {
	return find_class_users(p_class_name);
}

Vector<String> GritGdxApply::find_class_users(const String &p_class_name) {
	Vector<String> users;
	const String needle = "type=\"" + p_class_name + "\"";
	for (const String &scene_path : collect_scenes("res://")) {
		Error error = OK;
		const String text = FileAccess::get_file_as_string(scene_path, &error);
		if (error == OK && text.contains(needle)) {
			users.push_back(scene_path);
		}
	}
	return users;
}

Error GritGdxApply::remove_directory(const String &p_path) {
	Ref<DirAccess> directory = DirAccess::open(p_path);
	if (directory.is_null()) {
		return ERR_CANT_OPEN;
	}
	directory->set_include_hidden(true);
	for (const String &name : directory->get_directories()) {
		remove_directory(p_path.path_join(name));
	}
	for (const String &name : directory->get_files()) {
		directory->remove(name);
	}
	return DirAccess::remove_absolute(p_path);
}

// ---------------------------------------------------------------------------
// add flow
// ---------------------------------------------------------------------------

String GritGdxApply::Plan::summary() const {
	const int total = converted_functions + skipped_functions();
	if (skipped.is_empty()) {
		return vformat("converts all %d method(s)", total);
	}
	return vformat("converts %d of %d methods", converted_functions, total);
}

String GritGdxApply::Plan::skipped_text() const {
	StringBuilder text;
	for (const GritGdxSkipped &entry : skipped) {
		text.append(vformat("  %s: %s\n", entry.name, entry.reason));
	}
	return text.as_string();
}

Error GritGdxApply::plan(const String &p_script_path, Plan &r_plan, String &r_error) {
	const String script_path = p_script_path.begins_with("res://") ? p_script_path : ProjectSettings::get_singleton()->localize_path(p_script_path);
	Error load_error = OK;
	Ref<GDScript> script = GDScriptCache::get_full_script(script_path, load_error);
	if (load_error != OK || script.is_null()) {
		r_error = vformat("could not load \"%s\"", script_path);
		return load_error != OK ? load_error : ERR_CANT_OPEN;
	}
	if (script->get_global_name().is_empty()) {
		r_error = vformat("\"%s\" has no class_name; add one before converting, for example `class_name %s`.", script_path, GritGdxGenerator::suggested_class_name(script_path));
		return ERR_INVALID_PARAMETER;
	}
	GritGdxClass klass;
	// No extension directory: class-name collisions inside the target extension
	// are a naming detail, not a reason for a method to stay behind.
	const Error error = GritGdxGenerator::collect(script_path, String(), klass, r_error);
	if (error != OK) {
		return error;
	}
	const LocalVector<GritGdxSkipped> emitter_skipped = GritGdxEmitter::skipped_functions(klass);
	r_plan = Plan();
	r_plan.script_path = script_path;
	r_plan.class_name = klass.class_name;
	r_plan.base_type = String(klass.base_type);
	r_plan.converted_functions = int(klass.functions.size()) - int(emitter_skipped.size());
	r_plan.skipped = GritGdxGenerator::collect_skipped(klass);
	return OK;
}

Error GritGdxApply::prepare(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_allow_partial, GritGdxClass &r_class, Result &r_result, String &r_report) {
	Error error = OK;
	const String script_path = p_script_path.begins_with("res://") ? p_script_path : ProjectSettings::get_singleton()->localize_path(p_script_path);
	Ref<GDScript> script = GDScriptCache::get_full_script(script_path, error);
	if (error != OK || script.is_null()) {
		r_report = vformat("Grit: could not load \"%s\".", script_path);
		return error != OK ? error : ERR_CANT_OPEN;
	}

	const String directory = p_extension_directory.ends_with("/") ? p_extension_directory.left(-1) : p_extension_directory;
	r_result = Result();
	r_result.script_path = script_path;
	r_result.extension_directory = directory;
	r_result.extension_name = GritGdxManifest::extension_name(directory);
	r_result.scene_path = p_scene_path;
	r_result.node_path = p_node_path;

	// A conversion is class-wide: the script's global class becomes the generated
	// class, so the script must own one. Without it the generated name would be
	// derived from the path and could not replace the script's type references.
	if (script->get_global_name().is_empty()) {
		r_report = vformat("Grit: \"%s\" has no class_name. Add one before converting, for example `class_name %s`.", script_path, GritGdxGenerator::suggested_class_name(script_path));
		return ERR_INVALID_PARAMETER;
	}

	error = GritGdxGenerator::generate_class(directory, script_path, r_class, r_report);
	if (error != OK) {
		return error;
	}

	const LocalVector<GritGdxSkipped> skipped = GritGdxGenerator::collect_skipped(r_class);
	r_result.skipped_functions = skipped.size();
	StringBuilder skipped_report;
	for (const GritGdxSkipped &entry : skipped) {
		skipped_report.append(vformat("    %s: %s\n", entry.name, entry.reason));
	}
	r_result.skipped_report = skipped_report.as_string();
	r_result.class_name = r_class.class_name;

	if (r_result.skipped_functions > 0) {
		// A class with no convertible method has nothing to offer; a partial
		// class is allowed when the caller asked for it (the report still names
		// what stays behind, and Convert Back restores the script).
		const int converted = int(r_class.functions.size()) - int(GritGdxEmitter::skipped_functions(r_class).size());
		StringBuilder text;
		text.append(r_report);
		if (converted <= 0) {
			text.append(vformat("\n  none of the %d function(s) can be converted, so nothing was built and the project was not changed:\n", r_result.skipped_functions));
			text.append(r_result.skipped_report);
			text.append("  rewrite or remove those functions, then try again\n");
			r_report = text.as_string();
			return ERR_INVALID_DATA;
		}
		if (!p_allow_partial) {
			text.append(vformat("\n  %d function(s) cannot be converted, so nothing was built and the project was not changed:\n", r_result.skipped_functions));
			text.append(r_result.skipped_report);
			text.append("  rewrite or remove those functions, then try again\n");
			r_report = text.as_string();
			return ERR_INVALID_DATA;
		}
		text.append(vformat("\n  converting %d of %d methods; these stay behind as stubs that log an error when called:\n", converted, converted + r_result.skipped_functions));
		text.append(r_result.skipped_report);
		text.append("  Convert Back restores the script at any time\n");
		r_report = text.as_string();
	}
	return OK;
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
	return OK;
}

Error GritGdxApply::finish(const GritGdxClass &p_class, bool p_built, Result &r_result, String &r_report) {
	// A conversion is class-wide: every node in every scene that uses the script
	// switches to the generated class. The archive replaces the script for all of
	// them, so converting just the clicked node would leave the others on a `.gd`
	// that is about to be renamed.
	bool rewritten = false;
	for (const String &scene : collect_scenes("res://")) {
		bool scene_rewritten = false;
		String rewrite_error;
		const Error rewrite_status = convert_node(scene, String(), p_class.script_path, p_class.class_name, scene_rewritten, rewrite_error);
		if (rewrite_status != OK || !rewrite_error.is_empty()) {
			r_report = r_report + "\nGrit: " + (rewrite_error.is_empty() ? "could not rewrite a scene" : rewrite_error);
			return rewrite_status == OK ? ERR_CANT_CREATE : rewrite_status;
		}
		rewritten = rewritten || scene_rewritten;
	}
	r_result.node_rewritten = rewritten;
	if (!rewritten) {
		// A class can be converted without being used by a scene node yet: the
		// archive is the source and scripts refer to the class by name.
		r_report = r_report + "\nGrit: no scene node uses this class; only the archive and the library were written.";
	}

	// The converted node now references the archive, so it has to exist. When no
	// node needs the live script anymore it is renamed; when other nodes still use
	// it, a copy is archived and those nodes keep the `.gd`.
	const int remaining = count_script_users(p_class.script_path);
	const String archived = p_class.script_path.get_basename() + ".gds";
	if (archived != p_class.script_path && FileAccess::exists(p_class.script_path)) {
		close_editor_script_tabs(p_class.script_path);
		if (FileAccess::exists(archived)) {
			DirAccess::remove_absolute(archived);
		}
		Error archive_error = OK;
		if (remaining == 0) {
			archive_error = DirAccess::rename_absolute(p_class.script_path, archived);
			// Keep the script's UID with it so a later Convert Back restores the
			// same resource id.
			if (FileAccess::exists(p_class.script_path + ".uid")) {
				DirAccess::rename_absolute(p_class.script_path + ".uid", archived + ".uid");
			}
		} else {
			archive_error = DirAccess::copy_absolute(p_class.script_path, archived);
		}
		if (archive_error != OK) {
			r_report = r_report + vformat("\nGrit: could not archive \"%s\".", p_class.script_path);
			return archive_error;
		}
	}
	if (FileAccess::exists(archived)) {
		r_result.archived_script = archived;

		GritGdxManifest manifest;
		if (manifest.load(r_result.extension_directory) == OK) {
			if (const GritGdxManifestEntry *entry = manifest.find_class(p_class.class_name)) {
				GritGdxManifestEntry updated = *entry;
				updated.archived_script = archived;
				// Baseline for the stale check: edits to the archive after this
				// point mean the library no longer matches the source.
				Error hash_error = OK;
				const String source = FileAccess::get_file_as_string(archived, &hash_error);
				updated.source_hash = hash_error == OK ? source.sha256_text() : String();
				manifest.upsert(updated);
				manifest.save(r_result.extension_directory);
			}
		}
	}

	StringBuilder report;
	report.append(r_report);
	report.append(vformat("\n  class: %s (in %s)\n", p_class.class_name, r_result.extension_directory));
	report.append(vformat("  node rewritten: %s\n", r_result.node_rewritten ? "yes" : "no"));
	if (!r_result.archived_script.is_empty()) {
		report.append(vformat("  script archived: %s\n", r_result.archived_script));
	}
	if (remaining > 0) {
		report.append(vformat("  script kept for %d other node(s): %s\n", remaining, p_class.script_path));
	}
	if (!r_result.library_path.is_empty()) {
		report.append(vformat("  library: %s\n", r_result.library_path));
	}
	report.append("  the editor reloads the scene and the extension\n");
	r_report = report.as_string();
	return OK;
}

Error GritGdxApply::run(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_build, bool p_allow_partial, Result &r_result, String &r_report) {
	GritGdxClass klass;
	Error error = prepare(p_script_path, p_extension_directory, p_scene_path, p_node_path, p_allow_partial, klass, r_result, r_report);
	if (error != OK) {
		return error;
	}
	if (p_build) {
		String build_error;
		String library;
		const Error build_status = build(r_result.extension_directory, "template_debug", Progress(), library, build_error);
		if (build_status != OK) {
			r_report = r_report + "\nGrit: the build failed:\n" + build_error;
			return build_status;
		}
		r_result.built = true;
		r_result.library_path = library;
	}
	return finish(klass, p_build, r_result, r_report);
}

// ---------------------------------------------------------------------------
// legacy (.gde) archive migration
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

bool GritGdxApply::has_legacy_archive(const String &p_class_name) {
	GritGdxApply::RevertTarget target;
	return GritGdxApply::find_revert_target(p_class_name, target) && target.archived_script.ends_with(".gde");
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
			if (!entry.archived_script.ends_with(".gde")) {
				continue;
			}
			if (!FileAccess::exists(entry.archived_script)) {
				report.append(vformat("Grit: \"%s\" has no legacy archive at \"%s\"; skipped.\n", entry.class_name, entry.archived_script));
				continue;
			}
			const String legacy = entry.archived_script;
			const String archived = entry.script_path.get_basename() + ".gds";
			if (FileAccess::exists(archived)) {
				report.append(vformat("Grit: \"%s\" already exists; skipped \"%s\".\n", archived, entry.class_name));
				continue;
			}
			const Error rename_error = DirAccess::rename_absolute(legacy, archived);
			if (rename_error != OK) {
				report.append(vformat("Grit: could not rename \"%s\"; skipped \"%s\".\n", legacy, entry.class_name));
				continue;
			}
			if (FileAccess::exists(legacy + ".uid")) {
				DirAccess::rename_absolute(legacy + ".uid", archived + ".uid");
			}

			// The legacy archive was attached to the class's nodes; detach them.
			int detached = 0;
			for (const String &scene : collect_scenes("res://")) {
				String detach_error;
				detached += detach_archive_assignments(scene, entry.class_name, legacy, detach_error);
				if (!detach_error.is_empty()) {
					report.append("Grit: " + detach_error + "\n");
				}
			}

			entry.archived_script = archived;
			// The legacy manifest predates the stale baseline; record it now.
			Error hash_error = OK;
			const String source = FileAccess::get_file_as_string(archived, &hash_error);
			entry.source_hash = hash_error == OK ? source.sha256_text() : String();
			changed = true;
			r_migrated.push_back(entry.class_name);
			report.append(vformat("Grit: migrated \"%s\": %s -> %s (%d node(s) detached).\n", entry.class_name, legacy, archived, detached));
		}
		if (changed) {
			const Error save_error = manifest.save(directory);
			if (save_error != OK) {
				report.append(vformat("Grit: could not write the manifest in \"%s\".\n", directory));
				continue;
			}
			// Re-write the descriptor so the migrated extension gets its class icon.
			GritGdxGenerator::write_extension_files(directory);
		}
	}
	if (r_migrated.is_empty()) {
		report.append("Grit: nothing to migrate (no legacy .gde archive found).\n");
	}
	r_report = report.as_string();
	return OK;
}

// ---------------------------------------------------------------------------
// rebuild from the archive
// ---------------------------------------------------------------------------

Error GritGdxApply::begin_rebuild(const String &p_class_name, const String &p_extension_directory, Rebuild &r_rebuild) {
	r_rebuild = Rebuild();
	String extension = p_extension_directory;
	if (extension.is_empty()) {
		for (const String &directory : GritGdxManifest::list_extensions()) {
			GritGdxManifest manifest;
			if (manifest.load(directory) == OK && manifest.has_class(p_class_name)) {
				extension = directory;
				break;
			}
		}
	}
	if (extension.is_empty()) {
		r_rebuild.report = vformat("Grit: could not find the extension that owns \"%s\".", p_class_name);
		r_rebuild.error = ERR_DOES_NOT_EXIST;
		return r_rebuild.error;
	}
	GritGdxManifest manifest;
	const Error manifest_error = manifest.load(extension);
	const GritGdxManifestEntry *entry = manifest_error == OK ? manifest.find_class(p_class_name) : nullptr;
	if (!entry) {
		r_rebuild.report = vformat("Grit: \"%s\" is not in \"%s\".", p_class_name, extension);
		r_rebuild.error = ERR_DOES_NOT_EXIST;
		return r_rebuild.error;
	}
	if (entry->archived_script.is_empty() || !FileAccess::exists(entry->archived_script)) {
		r_rebuild.report = vformat("Grit: \"%s\" has no archived source to rebuild from (%s).", p_class_name, entry->archived_script);
		r_rebuild.error = ERR_DOES_NOT_EXIST;
		return r_rebuild.error;
	}

	Error error = OK;
	const String source = FileAccess::get_file_as_string(entry->archived_script, &error);
	if (error != OK) {
		r_rebuild.report = vformat("Grit: could not read \"%s\".", entry->archived_script);
		r_rebuild.error = error;
		return error;
	}

	// The recorder compiles a GDScript at its resource path, and the archive must
	// never be compiled. Provide the archive text at the original `.gd` path for
	// the duration of the generation, then restore what was there.
	const String script_path = entry->script_path;
	const bool live_exists = FileAccess::exists(script_path);
	String live_source;
	if (live_exists) {
		live_source = FileAccess::get_file_as_string(script_path, &error);
		if (error != OK) {
			r_rebuild.report = vformat("Grit: could not read \"%s\".", script_path);
			r_rebuild.error = error;
			return error;
		}
	}
	DirAccess::make_dir_recursive_absolute(script_path.get_base_dir());
	{
		Ref<FileAccess> file = FileAccess::open(script_path, FileAccess::WRITE);
		if (file.is_null()) {
			r_rebuild.report = vformat("Grit: could not write \"%s\".", script_path);
			r_rebuild.error = ERR_CANT_CREATE;
			return r_rebuild.error;
		}
		file->store_string(source);
	}

	r_rebuild.result.script_path = script_path;
	r_rebuild.result.class_name = p_class_name;
	r_rebuild.result.extension_directory = extension;
	r_rebuild.result.extension_name = GritGdxManifest::extension_name(extension);
	r_rebuild.result.archived_script = entry->archived_script;

	const Error generate_error = GritGdxGenerator::generate_class(extension, script_path, r_rebuild.klass, r_rebuild.report);

	// Restore the live script, or remove the scratch `.gd` that only existed for
	// the generation (the archive is the source of a rebuilt class).
	if (live_exists) {
		Ref<FileAccess> file = FileAccess::open(script_path, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(live_source);
		}
	} else {
		DirAccess::remove_absolute(script_path);
		DirAccess::remove_absolute(script_path + ".uid");
	}
	r_rebuild.error = generate_error;
	return generate_error;
}

Error GritGdxApply::finish_rebuild(Rebuild &r_rebuild, bool p_built, String &r_report) {
	if (r_rebuild.error != OK) {
		r_report = r_rebuild.report;
		return r_rebuild.error;
	}
	// The new baseline is the archive the sources were generated from. Only a
	// built library matches it, so a source-only run keeps the class stale.
	if (p_built) {
		Error error = OK;
		const String source = FileAccess::get_file_as_string(r_rebuild.result.archived_script, &error);
		GritGdxManifest manifest;
		if (error == OK && manifest.load(r_rebuild.result.extension_directory) == OK) {
			if (const GritGdxManifestEntry *entry = manifest.find_class(r_rebuild.klass.class_name)) {
				GritGdxManifestEntry updated = *entry;
				updated.archived_script = r_rebuild.result.archived_script;
				updated.source_hash = source.sha256_text();
				manifest.upsert(updated);
				manifest.save(r_rebuild.result.extension_directory);
			}
		}
	}

	StringBuilder report;
	report.append(r_rebuild.report);
	report.append(vformat("\n  class: %s (in %s)\n", r_rebuild.klass.class_name, r_rebuild.result.extension_directory));
	report.append(vformat("  rebuilt from: %s\n", r_rebuild.result.archived_script));
	if (p_built) {
		report.append("  library: " + r_rebuild.result.library_path + "\n");
	} else {
		report.append("  the library was not rebuilt; the class is still out of date\n");
	}
	r_report = report.as_string();
	return OK;
}

Error GritGdxApply::rebuild(const String &p_class_name, const String &p_extension_directory, bool p_build, Result &r_result, String &r_report) {
	Rebuild state;
	Error error = begin_rebuild(p_class_name, p_extension_directory, state);
	if (error != OK) {
		r_report = state.report;
		r_result = state.result;
		return error;
	}
	bool built = false;
	if (p_build) {
		String build_output;
		String library;
		const Error build_status = build(state.result.extension_directory, "template_debug", Progress(), library, build_output);
		if (build_status != OK) {
			state.error = build_status;
			state.report = state.report + "\nGrit: the build failed:\n" + build_output;
		} else {
			built = true;
			state.result.built = true;
			state.result.library_path = library;
		}
	}
	const Error finish_status = finish_rebuild(state, built, r_report);
	r_result = state.result;
	return finish_status;
}

// ---------------------------------------------------------------------------
// revert target lookup
// ---------------------------------------------------------------------------

bool GritGdxApply::find_revert_target(const String &p_class_name, RevertTarget &r_target) {
	for (const String &directory : GritGdxManifest::list_extensions()) {
		GritGdxManifest manifest;
		if (manifest.load(directory) != OK) {
			continue;
		}
		if (const GritGdxManifestEntry *entry = manifest.find_class(p_class_name)) {
			r_target = RevertTarget();
			r_target.extension_directory = directory;
			r_target.script_path = entry->script_path;
			r_target.base_type = entry->base_type;
			r_target.archived_script = entry->archived_script;
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// revert
// ---------------------------------------------------------------------------

// Nodes using a script whose type is not the generated class: deleting that script
// for a revert would break them.
int GritGdxApply::script_users_outside_class(const String &p_script_path, const String &p_class_name) {
	int users = 0;
	for (const String &scene_path : collect_scenes("res://")) {
		Error error = OK;
		const String text = FileAccess::get_file_as_string(scene_path, &error);
		if (error != OK || !text.contains(p_script_path)) {
			continue;
		}
		const PackedStringArray lines = text.split("\n");
		const String id = script_resource_id(lines, p_script_path);
		if (id.is_empty()) {
			continue;
		}
		const String assignment = "script = ExtResource(\"" + id + "\")";
		for (const SceneNodeInfo &node : scene_nodes(lines)) {
			const String header = lines[node.line];
			const int type_position = header.find("type=\"");
			if (type_position < 0) {
				continue;
			}
			const int value_start = type_position + 6;
			const int value_end = header.find("\"", value_start);
			if (value_end < 0) {
				continue;
			}
			if (header.substr(value_start, value_end - value_start) == p_class_name) {
				continue;
			}
			for (int i = node.line + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
				if (lines[i].strip_edges() == assignment) {
					users++;
					break;
				}
			}
		}
	}
	return users;
}

bool GritGdxApply::has_replacement_script(const String &p_class_name, bool &r_identical) {
	r_identical = false;
	for (const String &directory : GritGdxManifest::list_extensions()) {
		GritGdxManifest manifest;
		if (manifest.load(directory) != OK) {
			continue;
		}
		const GritGdxManifestEntry *entry = manifest.find_class(p_class_name);
		if (!entry) {
			continue;
		}
		if (entry->archived_script.is_empty() || !FileAccess::exists(entry->archived_script) || !FileAccess::exists(entry->script_path)) {
			return false;
		}
		Error archive_error = OK;
		Error live_error = OK;
		const String archived = FileAccess::get_file_as_string(entry->archived_script, &archive_error);
		const String live = FileAccess::get_file_as_string(entry->script_path, &live_error);
		if (archive_error != OK || live_error != OK) {
			return false;
		}
		r_identical = archived == live;
		return true;
	}
	return false;
}

Error GritGdxApply::revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_replace_script, bool p_build, const Progress &p_progress, Result &r_result, String &r_report) {
	RevertTarget target;
	bool found = false;
	if (!p_extension_directory.is_empty()) {
		GritGdxManifest manifest;
		const String directory = p_extension_directory.ends_with("/") ? p_extension_directory.left(-1) : p_extension_directory;
		if (manifest.load(directory) == OK) {
			if (const GritGdxManifestEntry *entry = manifest.find_class(p_class_name)) {
				target.extension_directory = directory;
				target.script_path = entry->script_path;
				target.base_type = entry->base_type;
				target.archived_script = entry->archived_script;
				found = true;
			}
		}
	}
	if (!found) {
		found = find_revert_target(p_class_name, target);
	}
	if (!found) {
		r_report = vformat("Grit: could not find the extension that owns \"%s\".", p_class_name);
		return ERR_DOES_NOT_EXIST;
	}

	r_result = Result();
	r_result.class_name = p_class_name;
	r_result.extension_directory = target.extension_directory;
	r_result.extension_name = GritGdxManifest::extension_name(target.extension_directory);
	r_result.script_path = target.script_path;
	r_result.archived_script = target.archived_script;

	// Put the archive back where the script was. A `.gd` at that path is a script
	// added after the conversion: it is replaced when it is identical, or when the
	// caller opted in; otherwise Convert Back refuses instead of overwriting code.
	if (!target.archived_script.is_empty() && FileAccess::exists(target.archived_script) && FileAccess::exists(target.script_path)) {
		Error archive_error = OK;
		Error live_error = OK;
		const String archived_text = FileAccess::get_file_as_string(target.archived_script, &archive_error);
		const String live_text = FileAccess::get_file_as_string(target.script_path, &live_error);
		const bool identical = archive_error == OK && live_error == OK && archived_text == live_text;
		if (!identical && !p_replace_script) {
			r_report = vformat("Grit: \"%s\" already exists; Convert Back would replace it. Delete or rename it first, or convert back with replace enabled.", target.script_path);
			return ERR_INVALID_PARAMETER;
		}
		const int foreign_users = GritGdxApply::script_users_outside_class(target.script_path, p_class_name);
		if (foreign_users > 0) {
			r_report = vformat("Grit: \"%s\" is attached to %d other node(s); deleting it would break them.", target.script_path, foreign_users);
			return ERR_INVALID_PARAMETER;
		}
		close_editor_script_tabs(target.script_path);
		DirAccess::remove_absolute(target.script_path);
		DirAccess::remove_absolute(target.script_path + ".uid");
	}
	if (!target.archived_script.is_empty() && FileAccess::exists(target.archived_script) && !FileAccess::exists(target.script_path)) {
		const Error rename_error = DirAccess::rename_absolute(target.archived_script, target.script_path);
		if (rename_error != OK) {
			r_report = vformat("Grit: could not restore \"%s\".", target.script_path);
			return rename_error;
		}
		// The archive carried the script's UID; restore it with the script.
		if (FileAccess::exists(target.archived_script + ".uid")) {
			DirAccess::rename_absolute(target.archived_script + ".uid", target.script_path + ".uid");
		}
	}

	// A conversion is class-wide, so Convert Back restores every node of the class
	// in every scene. Collect the paths first: restore_node() inserts an
	// ExtResource line, which shifts later line numbers.
	bool restored = false;
	for (const String &scene : collect_scenes("res://")) {
		Error read_error = OK;
		const String text = FileAccess::get_file_as_string(scene, &read_error);
		if (read_error != OK) {
			continue;
		}
		const PackedStringArray lines = text.split("\n");
		Vector<String> class_nodes;
		for (const SceneNodeInfo &node : scene_nodes(lines)) {
			const String line = lines[node.line];
			const int type_position = line.find("type=\"");
			if (type_position < 0) {
				continue;
			}
			const int value_start = type_position + 6;
			const int value_end = line.find("\"", value_start);
			if (value_end < 0 || line.substr(value_start, value_end - value_start) != p_class_name) {
				continue;
			}
			class_nodes.push_back(node_scene_path(node));
		}
		for (const String &node_path : class_nodes) {
			String restore_error;
			const Error restore_status = restore_node(scene, node_path, target.script_path, target.base_type, restore_error);
			if (restore_status != OK) {
				r_report = "Grit: " + restore_error;
				return restore_status;
			}
			restored = true;
		}
	}
	r_result.node_rewritten = restored;

	// Named extension: drop the class only when nothing else instantiates it.
	Vector<String> users = find_class_users(p_class_name);
	if (!p_scene_path.is_empty() && !p_node_path.is_empty() && !users.is_empty()) {
		Error read_error = OK;
		const String text = FileAccess::get_file_as_string(p_scene_path, &read_error);
		if (read_error == OK) {
			const PackedStringArray lines = text.split("\n");
			const Vector<SceneNodeInfo> nodes = scene_nodes(lines);
			const int header = find_node_line(nodes, p_node_path);
			if (header >= 0) {
				users.remove_at(users.find(p_scene_path));
			}
		}
	}

	bool removed_class = false;
	if (users.is_empty()) {
		const Error remove_status = GritGdxGenerator::remove_class(target.extension_directory, p_class_name);
		if (remove_status != OK && remove_status != ERR_DOES_NOT_EXIST) {
			r_report = vformat("Grit: could not remove \"%s\" from the extension.", p_class_name);
			return remove_status;
		}
		removed_class = true;
	}

	GritGdxManifest manifest;
	manifest.load(target.extension_directory);
	const bool empty = manifest.classes.is_empty();
	bool removed_extension = false;
	if (removed_class && empty) {
		removed_extension = remove_directory(target.extension_directory) == OK;
	}

	if (p_build && removed_class && !empty) {
		String build_error;
		String library;
		const Error build_status = build(target.extension_directory, "template_debug", p_progress, library, build_error);
		if (build_status != OK) {
			r_report = "Grit: the class was removed but the rebuild failed:\n" + build_error;
			return build_status;
		}
		r_result.built = true;
		r_result.library_path = library;
	}

	StringBuilder report;
	report.append(vformat("Grit: reverted \"%s\" to a script node\n", p_class_name));
	report.append(vformat("  script: %s\n", target.script_path));
	report.append(vformat("  node rewritten: %s\n", r_result.node_rewritten ? "yes" : "no"));
	if (removed_class) {
		report.append(vformat("  class removed from %s%s\n", target.extension_directory, removed_extension ? " (extension deleted, no classes left)" : ""));
	} else {
		report.append(vformat("  class kept in %s: %d scene(s) still use it\n", target.extension_directory, users.size()));
	}
	if (r_result.built) {
		report.append(vformat("  library: %s\n", r_result.library_path));
	}
	report.append("  the editor reloads the scene and the extension\n");
	r_report = report.as_string();
	return OK;
}
