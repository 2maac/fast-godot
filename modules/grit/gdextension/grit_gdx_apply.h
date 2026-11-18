#pragma once

#include "grit_gdx_class.h"

#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <functional>

// Drives the scene-tree actions:
//
//   add:     prepare() -> build() -> finish()
//   revert:  revert()
//
// prepare() and finish()/revert() touch the resource system and stay on the main
// thread; only build() runs on a worker thread.
class GritGdxApply {
public:
	using Progress = std::function<void(int, const String &)>;

	struct Result {
		String script_path;
		String class_name;
		String extension_directory;
		String extension_name;
		String library_path;
		String scene_path;
		String node_path;
		String archived_script;
		bool built = false;
		bool node_rewritten = false;
		int skipped_functions = 0;
		String skipped_report;
	};

	// What converting a script would produce, without writing or building
	// anything: the class name, how many methods convert, and which ones stay
	// behind with the reason. Used by the pre-flight dialog and the CLI.
	struct Plan {
		String script_path;
		String class_name;
		String base_type;
		int converted_functions = 0;
		LocalVector<GritGdxSkipped> skipped;

		int skipped_functions() const { return skipped.size(); }
		bool is_partial() const { return !skipped.is_empty(); }
		// One line, e.g. "converts 7 of 9 methods".
		String summary() const;
		// Multi-line list of the methods that stay behind.
		String skipped_text() const;
	};
	static Error plan(const String &p_script_path, Plan &r_plan, String &r_error);

	// Main thread. Adds the script's class to the extension (creating it).
	// `p_node_path` is relative to the edited scene root ("." is the root); an
	// empty path converts every node that uses the script. With
	// `p_allow_partial` the class is generated even when some methods cannot be
	// translated (they are reported and simply missing from the class).
	static Error prepare(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_allow_partial, GritGdxClass &r_class, Result &r_result, String &r_report);

	// Worker thread.
	static Error build(const String &p_extension_directory, const String &p_target, const Progress &p_progress, String &r_library, String &r_error);

	// Main thread. Rewrites the node, archives the script when it is the only
	// node using it, and records the archive in the manifest.
	static Error finish(const GritGdxClass &p_class, bool p_built, Result &r_result, String &r_report);

	// Main thread. Returns one class to a script node. Refused while any node
	// still uses the class.
	// Creates (or regenerates) the GDExtension for a `.gde` source. The extension
	// lives next to the source in `<stem>_gdext/<stem>.gdextension`; nothing is
	// attached to a node and no scene is rewritten.
	static String project_extension_directory() { return "res://gdext"; }
	static Error create_source(const String &p_script_path, String &r_report, String *r_extension_directory = nullptr);
	// Regenerates every `.gde` class in the project into the single extension and
	// prunes classes whose source is gone. No build.
	static Error refresh_project_extension(String &r_report);
	// After a successful build: remove per-source extension folders that are fully
	// subsumed by the project extension.
	static void consolidate_project_extension(String &r_report);

	// The Build GDE button in the script editor: generates the open `.gde`
	// source's extension and builds it. It never touches a scene or a node.
	static void build_source_from_editor(const String &p_script_path);
	static void poll_build_source();
	static void create_source_callback(const String &p_script_path);

	static Error revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_build, const Progress &p_progress, Result &r_result, String &r_report);

	// Upgrades archives from the legacy format (`.gde`, attached to the node) to
	// the current one (`.gds`, detached), updates the manifest and its hash, and
	// re-writes the descriptor (which adds the class icon). Idempotent.
	static Error migrate_legacy(Vector<String> &r_migrated, String &r_report);
	// True when the class still uses a legacy `.gde` archive.
	static bool has_legacy_archive(const String &p_class_name);
	// Scene-tree badge provider: returns {"icon": "GritGdxStaleBadge", "tooltip":
	// ...} when the class's archive changed since the library was built.
	static Variant class_stale_badge(const StringName &p_class);

	// A `class_name` to offer for a script that has none: the sanitized PascalCase
	// file stem, de-collided against native classes and global script classes.
	static String suggested_class_name(const String &p_script_path);
	// Inserts `class_name <suggested>` at the top level (after comments and
	// annotations, before the first statement). Returns the used name; a script
	// that already has one is left untouched.
	static Error add_class_name(const String &p_script_path, String &r_class_name, String &r_error);

	// Scenes that instantiate a generated class (used to gate revert and the
	// context menu).
	static Vector<String> class_users(const String &p_class_name);

	// type="..." of a node as written in the scene file. The editor keeps the
	// scene it loaded in memory, so this is the only way to see a type it has not
	// reloaded yet.
	static String scene_node_type(const String &p_scene_path, const String &p_node_path);

	// Where a generated class lives and what it has to be restored to.
	struct RevertTarget {
		String extension_directory;
		String script_path;
		String base_type;
		String archived_script;
	};
	static bool find_revert_target(const String &p_class_name, RevertTarget &r_target);

	// prepare + build + finish, for the command line.
	static Error run(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_build, bool p_allow_partial, Result &r_result, String &r_report);

	// Main thread only. Closes the script editor tab for a path that a conversion
	// or revert is about to rename. No-op outside the editor; must not be called
	// from a worker thread (revert runs on one).
	static void close_editor_script_tabs(const String &p_path);

	// Rebuilding an already converted class from its `.gde` archive. The archive
	// is temporarily written to the original `.gd` path so the recorder can
	// compile it (the archive itself is intentionally never compiled); the live
	// script is restored afterwards when other nodes still use it.
	struct Rebuild {
		GritGdxClass klass;
		Result result;
		String report;
		Error error = OK;
	};
	// Main thread: regenerate the sources from the archive. No build, no scene
	// rewrite; `r_rebuild.error` is set when the class cannot be found or the
	// archive is missing.
	static Error begin_rebuild(const String &p_class_name, const String &p_extension_directory, Rebuild &r_rebuild);
	// Main thread, after the build: records the archive hash (clearing "stale")
	// and writes the report. Only a built class records a new hash.
	static Error finish_rebuild(Rebuild &r_rebuild, bool p_built, String &r_report);
	// Blocking convenience for the CLI: begin + build + finish.
	static Error rebuild(const String &p_class_name, const String &p_extension_directory, bool p_build, Result &r_result, String &r_report);

private:
	static Vector<String> collect_scenes(const String &p_directory);
	// Number of nodes that attach the script; optionally reports where they are.
	static int count_script_users(const String &p_script_path, Vector<String> *r_scenes = nullptr);
	// Nodes using the script whose type is not the class; deleting the script for
	// a revert would break them.
	static int script_users_outside_class(const String &p_script_path, const String &p_class_name);
	// Scenes that instantiate a generated class.
	static Vector<String> find_class_users(const String &p_class_name);
	static Error remove_directory(const String &p_path);
};
