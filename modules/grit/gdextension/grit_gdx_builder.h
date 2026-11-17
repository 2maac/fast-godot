#pragma once

#include "core/string/ustring.h"
#include "core/templates/list.h"

// Builds generated GDExtension projects.
//
// godot-cpp is fetched once and built against this engine's own API dump, then
// cached in the toolchain folder (or in <data>/grit-tools without one), so a
// later class only has to compile its own sources.
class GritGdxBuilder {
public:
	// <toolchain>/godot-cpp, or <data>/grit-tools/godot-cpp. `GRIT_GDX_GODOT_CPP`
	// overrides it (used by the lab).
	static String godot_cpp_root();
	// Resolves the godot-cpp root and the API dump on the main thread and caches
	// them, so the worker thread never reads editor settings.
	static void resolve_paths();
	// Cached extension_api.json for this engine build; generated on first use.
	// Falls back to the API file the pinned godot-cpp ships when the editor's
	// data directory is not writable, and returns a path that may not exist so
	// build() can report it.
	static String api_json_path();
	// Makes sure godot-cpp is present and has a static library for `p_target`.
	static bool ensure_godot_cpp(const String &p_target, String &r_error);
	// Builds the GDExtension library for `p_target` and returns its path.
	static bool build(const String &p_project_directory, const String &p_library_name, const String &p_target, String &r_library, String &r_error);
	// bin/lib<name>.<platform>.<arch>.<debug|release>.<dylib|so|dll>
	static String library_path(const String &p_project_directory, const String &p_library_name, const String &p_target);
	static String static_library_path(const String &p_target);

private:
	static String cached_root;
	static String cached_api;

	static bool run(const String &p_executable, const List<String> &p_arguments, String &r_output, int &r_exit_code, String &r_error);
	static bool run_scons(const String &p_directory, const List<String> &p_arguments, String &r_output, String &r_error);
	static String find_scons_module(const String &p_toolchain);
	static String shell_quote(const String &p_text);
};
