#pragma once

#include "core/string/ustring.h"
#include "core/templates/vector.h"

// Checks that everything needed to build a compiled export template is present.
//
// This is deliberately report-only for now: it walks the list on editor startup,
// says what is there and what is missing, and shows a non-cancellable progress
// dialog when something is missing. Downloads are the next step; the settings it
// reads (URLs, mirrors, checksums, proxy, offline) exist already so a source can
// be redirected without a rebuild when a host goes away.
class GritProvisioner {
public:
	struct Item {
		String id;
		String label;
		String state; // "present", "missing", or "manual".
		String detail;
	};

	// Registers the grit/provision/* editor settings. Safe to call more than once.
	static void register_settings();

	// Walks the checklist. Called when the export window opens (not at startup), and
	// once per session. Shows a dialog only when something is missing.
	static void run_probe();

	// The checklist, evaluated. Also used by the export plugin before a build.
	static Vector<Item> collect();

	// When the user points grit/toolchain/root at a self-contained toolchain folder,
	// this wires the editor to it: engine_root, the SCons launcher and the SCons
	// arguments the Windows build needs. Returns false when no such folder is set
	// or it is missing a piece.
	static bool apply_toolchain_root();

	// The interpreter that runs tools/grit_build_template.py for this machine.
	static String python_for_build();

	// Records the interpreter, SCons package, PATH additions and SCons options the
	// build helper must use, in <engine>/bin/grit/build_env.json.
	static void write_build_env(const String &p_engine_root);

private:
	static bool _has_engine_tree(String &r_detail);
	static bool _has_scons(String &r_detail);
	static bool _has_compiler(String &r_detail);
	static bool _has_moltenvk(String &r_detail);
};
