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
	file->store_string(output.as_string());
	return OK;
}

// Replaces the node's type and keeps its script attached: the assignment now
// points at the archived source (`.gde`), which loads as an inert GritGdxScript.
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
		// The editor writes script = ExtResource(...) after the node's other
		// properties, so look through the whole node block.
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

	Vector<int> converted;
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
		// Never replace a node that is already a generated class.
		const String current_type = line.substr(value_start, value_end - value_start);
		if (ClassDB::class_exists(current_type)) {
			const ClassDB::APIType api = ClassDB::get_api_type(current_type);
			if (api == ClassDB::API_EXTENSION || api == ClassDB::API_EDITOR_EXTENSION) {
				continue;
			}
		}
		lines.write[header] = line.substr(0, value_start) + p_class_name + line.substr(value_end);
		converted.push_back(header);
		r_rewritten = true;
	}
	if (!r_rewritten) {
		return OK;
	}

	// The assignment stays on each converted node; collect the lines to retarget.
	Vector<int> assignments;
	for (int header : converted) {
		for (int i = header + 1; i < lines.size() && !lines[i].begins_with("[node "); i++) {
			if (lines[i].strip_edges() == script_line) {
				assignments.push_back(i);
				break;
			}
		}
	}

	const String archived_path = p_script_path.get_basename() + ".gde";
	const int remaining = script_assignment_users(lines, id) - int(assignments.size());
	String archive_id = id;
	if (remaining > 0) {
		// Other nodes still run the live script. The converted nodes get their own
		// resource entry so they do not drag the others into the archive.
		archive_id = "grit_archive";
		for (int suffix = 2; text.contains("id=\"" + archive_id + "\""); suffix++) {
			archive_id = "grit_archive" + itos(suffix);
		}
		// Keep the new entry with the other resources rather than before the blank
		// line the editor writes under `[gd_scene]`.
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
		lines.insert(insert_at, "[ext_resource type=\"Script\" path=\"" + archived_path + "\" id=\"" + archive_id + "\"]");
		for (int &line_index : assignments) {
			if (line_index >= insert_at) {
				line_index++;
			}
		}
	} else {
		// Every user is converted: the single entry becomes the archive.
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].begins_with("[ext_resource") && lines[i].contains("path=\"" + p_script_path + "\"")) {
				lines.write[i] = retarget_resource_line(lines[i], archived_path);
				break;
			}
		}
	}
	const String archive_assignment = "script = ExtResource(\"" + archive_id + "\")";
	for (int line_index : assignments) {
		lines.write[line_index] = archive_assignment;
	}
	return write_scene(p_scene_path, lines, Vector<int>());
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
	const String archived_path = p_script_path.get_basename() + ".gde";

	// An entry for the live script may still exist (a shared script keeps it), in
	// which case it is reused. Otherwise the archive's entry is retargeted when no
	// other converted node depends on it, and a new entry is created if it is
	// shared.
	String id = script_resource_id(lines, p_script_path);
	if (id.is_empty()) {
		const String archive_id = script_resource_id(lines, archived_path);
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

	// A script shared by several nodes keeps living as a script: if it declares a
	// class name, that name would hide the generated class, so refuse.
	const int users = count_script_users(script_path);
	if (users > 1 && !script->get_global_name().is_empty()) {
		r_report = vformat("Grit: \"%s\" is attached to %d nodes and declares class_name \"%s\"; remove the class_name or give each node its own script.", script_path, users, script->get_global_name());
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
	// Rewrite the node(s) that used the script.
	if (!r_result.scene_path.is_empty()) {
		bool rewritten = false;
		String rewrite_error;
		const Error rewrite_status = convert_node(r_result.scene_path, r_result.node_path, p_class.script_path, p_class.class_name, rewritten, rewrite_error);
		if (rewrite_status != OK || !rewrite_error.is_empty()) {
			r_report = r_report + "\nGrit: " + (rewrite_error.is_empty() ? "could not rewrite the scene" : rewrite_error);
			return rewrite_status == OK ? ERR_CANT_CREATE : rewrite_status;
		}
		r_result.node_rewritten = rewritten;
		if (!rewritten) {
			r_report = r_report + "\nGrit: the node was not rewritten (it may already be a generated class).";
			return ERR_CANT_CREATE;
		}
	} else {
		// Command line without a node: rewrite every node using the script.
		for (const String &scene : collect_scenes("res://")) {
			bool rewritten = false;
			String rewrite_error;
			const Error rewrite_status = convert_node(scene, String(), p_class.script_path, p_class.class_name, rewritten, rewrite_error);
			if (rewrite_status != OK) {
				r_report = r_report + "\nGrit: " + rewrite_error;
				return rewrite_status;
			}
			r_result.node_rewritten = r_result.node_rewritten || rewritten;
		}
	}

	// The converted node now references the archive, so it has to exist. When no
	// node needs the live script anymore it is renamed; when other nodes still use
	// it, a copy is archived and those nodes keep the `.gd`.
	const int remaining = count_script_users(p_class.script_path);
	const String archived = p_class.script_path.get_basename() + ".gde";
	if (archived != p_class.script_path && FileAccess::exists(p_class.script_path)) {
		if (FileAccess::exists(archived)) {
			DirAccess::remove_absolute(archived);
		}
		Error archive_error = OK;
		if (remaining == 0) {
			archive_error = DirAccess::rename_absolute(p_class.script_path, archived);
			DirAccess::remove_absolute(p_class.script_path + ".uid");
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

Error GritGdxApply::revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_build, const Progress &p_progress, Result &r_result, String &r_report) {
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

	// Restore the script from the archive when it was archived.
	if (!target.archived_script.is_empty() && FileAccess::exists(target.archived_script) && !FileAccess::exists(target.script_path)) {
		const Error rename_error = DirAccess::rename_absolute(target.archived_script, target.script_path);
		if (rename_error != OK) {
			r_report = vformat("Grit: could not restore \"%s\".", target.script_path);
			return rename_error;
		}
	}

	// If the script was re-created next to its archive (the editor can do that),
	// the archive is a leftover copy of the converted state.
	if (!target.archived_script.is_empty() && FileAccess::exists(target.archived_script) && FileAccess::exists(target.script_path)) {
		DirAccess::remove_absolute(target.archived_script);
	}
	if (!target.archived_script.is_empty()) {
		// The `.gde` may have received a UID while it was attached.
		DirAccess::remove_absolute(target.archived_script + ".uid");
	}

	// Put the node back on the script.
	if (!p_scene_path.is_empty() && !p_node_path.is_empty()) {
		String restore_error;
		const Error restore_status = restore_node(p_scene_path, p_node_path, target.script_path, target.base_type, restore_error);
		if (restore_status != OK) {
			r_report = "Grit: " + restore_error;
			return restore_status;
		}
		r_result.node_rewritten = true;
	}

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
