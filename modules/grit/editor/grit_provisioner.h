#pragma once

#include "core/config/engine.h"
#include "core/object/object.h"

// Editor-side plumbing for compiling a project into an export template.
//
// The toolchain folder (Editor Settings > Grit > Toolchain) is per machine and
// self-contained: it carries the compilers and the engine source tree the export
// templates are built from. A checkout next to the editor is only used for a
// staged/dev build, which has to build against its own tree.
class GritProvisioner {
public:
	static constexpr const char *SETTING_TOOLCHAIN_ROOT = "grit/settings/toolchain_root_folder_path";

	// Registers grit/toolchain/root, which the export window and the build read.
	// Called once when the editor starts.
	static void register_settings();

	// Removes keys written by earlier versions (the per-source download settings and
	// the project-scoped engine root). They are no longer registered, so they would
	// otherwise show up as orphaned entries in the settings trees.
	static void purge_obsolete_settings();

	// The engine source tree this build must use: a configured folder, a checkout
	// next to the editor, or the copy the toolchain bundle carries. It never
	// downloads anything. Empty on failure, with r_error set.
	static String ensure_engine(String *r_error = nullptr);

	// Only used while building a toolchain: ensure_engine(), but if no source is on
	// disk it downloads this editor's source archive so the toolchain downloader
	// (which lives in the source tree) can run. That tree is then copied into the
	// new toolchain and deleted from the cache.
	static String ensure_engine_for_toolchain(const String &p_dest, String *r_error = nullptr);

	// The engine source tree the toolchain declares in its toolchain.json
	// ("engine": "engine/<revision>"), or empty when it declares none or the tree
	// is missing.
	static String toolchain_engine();

	// The toolchain folder chosen in the editor settings, or empty when unusable.
	static String configured_toolchain();

	// True when the configured toolchain folder has everything this platform needs.
	static bool toolchain_ready();

	// ~/fast_godot_toolchain (or %USERPROFILE%\fast_godot_toolchain).
	static String home_toolchain_folder();

	// Runs tools/toolchain/download_toolchain.{sh,cmd} from the engine source with a
	// progress dialog, then points the toolchain setting at p_dest.
	static bool download_toolchain(const String &p_dest, String *r_error = nullptr);

	// The interpreter that runs tools/grit_build.py.
	static String python_for_build();

	// Writes <engine>/bin/grit/build_env.json: interpreter, SCons package, PATH
	// additions, SCons options and the engine revision the build belongs to.
	static void write_build_env(const String &p_engine_root);

	// Runs at editor start and when the export window opens.
	static void run_probe();

	// Debug aid: GRIT_DOWNLOAD_NOW=1 creates the toolchain at startup, so the
	// bootstrap can be exercised headlessly.
	static void run_download_probe();
};
