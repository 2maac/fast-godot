#pragma once

#include "core/config/engine.h"
#include "core/object/object.h"

// Editor-side plumbing for compiling a project into an export template.
//
// Two places, deliberately:
//   Editor Settings > Grit > Toolchain    the toolchain folder (grit/toolchain/root)
//   Project Settings > Grit               engine_root, enabled, generated_directory
//
// The toolchain folder is per machine, not per project, which is why it is an editor
// setting; the rest describe the project.
class GritProvisioner {
public:
	static constexpr const char *SETTING_TOOLCHAIN_ROOT = "grit/settings/toolchain_root_folder_path";
	// Engine source folder, fetched with the "Fetch Source" button or chosen by hand.
	static constexpr const char *SETTING_SOURCE_PATH = "grit/settings/source_code_root_folder_path";

	// Registers grit/toolchain/root, which the export window and the build read.
	// Called once when the editor starts.
	static void register_settings();

	// Removes keys written by earlier versions (the per-source download settings and
	// the project-scoped engine root). They are no longer registered, so they would
	// otherwise show up as orphaned entries in the settings trees.
	static void purge_obsolete_settings();

	// Returns the engine source tree, fetching it for this editor's commit when it is
	// not on disk. Empty on failure, with r_error set. With p_show_progress the
	// download is shown and can be cancelled with Esc.
	static String ensure_engine(bool p_show_progress, String *r_error = nullptr);

	// The source folder chosen in Editor Settings, when it holds a tree.
	static String configured_source();

	// Downloads this platform's toolchain bundle into p_folder, unpacks it and
	// returns the folder that holds toolchain.json, or empty with r_error set.
	static String fetch_toolchain_to(const String &p_folder, bool p_show_progress, String *r_error = nullptr);

	// Downloads this editor's engine source into p_folder (or reuses a tree that is
	// already there) and returns the tree path, or an empty string with r_error set.
	// This is what the "Fetch Source" button calls.
	static String fetch_source_to(const String &p_folder, bool p_show_progress, String *r_error = nullptr);

	// The commit this editor was built from; the engine source is fetched for it.
	static String engine_commit();

	// The interpreter that runs tools/grit_build_template.py.
	static String python_for_build();

	// Writes <engine>/bin/grit/build_env.json: interpreter, SCons package, PATH
	// additions, SCons options and the engine revision the build belongs to.
	static void write_build_env(const String &p_engine_root);

	// Runs at editor start and when the export window opens.
	static void run_probe();
};
