#pragma once

#include "core/config/engine.h"
#include "core/object/object.h"

// Editor-side plumbing for compiling a project into an export template.
//
// Two things are configured, both in Project Settings under "grit/export":
//   toolchain_root  the unzipped Fast_Godot_Toolchain_* folder (Python, SCons and
//                   the platform toolchain); empty means "use the system".
//   engine_root     a local Fast_Godot checkout, for machines that cannot download
//                   the engine source. Empty means "fetch it for this build".
//
// Nothing here is an editor setting: those were the download-on-demand knobs of an
// earlier design and are gone.
class GritProvisioner {
public:
	static constexpr const char *SETTING_TOOLCHAIN_ROOT = "grit/export/toolchain_root";

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
