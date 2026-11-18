#pragma once

#include "grit_gdx_class.h"

#include "core/string/ustring.h"
#include "core/templates/vector.h"

#include <functional>

// Drives the GDExtension actions:
//
//   create/refresh:  .gde sources -> one project extension (no build)
//   build:           worker thread, publishes the descriptor after the library
//
// Generation touches the resource system and stays on the main thread; only
// build() runs on a worker thread.
class GritGdxApply {
public:
	using Progress = std::function<void(int, const String &)>;

	struct Result {
		String script_path;
		String extension_directory;
		String library_path;
		bool built = false;
	};

	// Worker thread. Builds the extension and publishes its descriptor only after
	// the library exists.
	static Error build(const String &p_extension_directory, const String &p_target, const Progress &p_progress, String &r_library, String &r_error);

	// The single extension every `.gde` source in the project generates into.
	static String project_extension_directory() { return "res://gdext"; }
	// Creates (or refreshes) the generated class of a `.gde` source inside the
	// project extension. Nothing is attached to a node and no scene is rewritten.
	static Error create_source(const String &p_script_path, String &r_report, String *r_extension_directory = nullptr);
	// Regenerates every `.gde` class in the project into the single extension and
	// prunes classes whose source is gone. No build.
	static Error refresh_project_extension(String &r_report);
	// After a successful build: remove per-source extension folders that are fully
	// subsumed by the project extension.
	static void consolidate_project_extension(String &r_report);

	// The Build GDE button in the script editor: regenerates the open `.gde`
	// source's class (and the rest of the project extension) and builds it. It
	// never touches a scene or a node.
	static void build_source_from_editor(const String &p_script_path);
	static void poll_build_source();
	static void create_source_callback(const String &p_script_path);

	// Upgrades classes recorded by the retired node-conversion flow: `class_name`
	// becomes `@gdextension_class_name(...)`, a legacy `.gde` attached archive
	// becomes a `.gde` source, and a missing source is restored from its `.gds`
	// snapshot. Idempotent.
	static Error migrate_legacy(Vector<String> &r_migrated, String &r_report);

	// Scene-tree badge provider: returns {"icon": "GritGdxStaleBadge", "tooltip":
	// ...} when the class's `.gde` source changed since the library was built.
	static Variant class_stale_badge(const StringName &p_class);

	// A `class_name` to offer for a source that has none: the sanitized PascalCase
	// file stem, de-collided against native classes and global script classes.
	static String suggested_class_name(const String &p_script_path);

private:
	static Vector<String> collect_scenes(const String &p_directory);
};
