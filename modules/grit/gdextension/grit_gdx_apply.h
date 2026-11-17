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
	static Error revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_build, const Progress &p_progress, Result &r_result, String &r_report);

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

private:
	static Vector<String> collect_scenes(const String &p_directory);
	// Number of nodes that attach the script; optionally reports where they are.
	static int count_script_users(const String &p_script_path, Vector<String> *r_scenes = nullptr);
	// Scenes that instantiate a generated class.
	static Vector<String> find_class_users(const String &p_class_name);
	static Error remove_directory(const String &p_path);
};
