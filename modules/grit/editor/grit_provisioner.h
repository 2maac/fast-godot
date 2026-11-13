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
	static constexpr const char *SETTING_TOOLCHAIN_ROOT = "grit/toolchain/root";

	// Registers grit/toolchain/root, which the export window and the build read.
	// Called once when the editor starts.
	static void register_settings();

	// Fetches the engine source for this editor's commit when it is not on disk and
	// reports what happened. With p_show_progress the download is shown and can be
	// cancelled with Esc.
	static bool ensure_engine(bool p_show_progress, String *r_error = nullptr);

	// Records the engine root for the build helper when a toolchain folder is used.
	static bool apply_toolchain_root();

	// The interpreter that runs tools/grit_build_template.py.
	static String python_for_build();

	// Writes <engine>/bin/grit/build_env.json: interpreter, SCons package, PATH
	// additions, SCons options and the engine revision the build belongs to.
	static void write_build_env(const String &p_engine_root);

	// Runs at editor start and when the export window opens.
	static void run_probe();
};
