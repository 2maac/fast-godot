#pragma once

#include "grit_gdx_class.h"
#include "grit_gdx_manifest.h"

// Turns GDScript files into classes of a generated GDExtension.
//
// An extension is a folder with a manifest, one set of class sources and a single
// registry; several scripts can be added to the same extension so the project
// ships one shared library.
class GritGdxGenerator {
public:
	// Adds or refreshes one class inside `p_extension_directory` (created when
	// missing) and regenerates the registry. `r_report` receives a summary.
	static Error generate_class(const String &p_extension_directory, const String &p_script_path, GritGdxClass &r_class, String &r_report);

	// Writes SConstruct, descriptor and registry from the folder's manifest.
	// `p_extra_class` is the class being generated: the manifest may not contain it
	// yet when the descriptor (and its [icons] section) is written.
	static Error write_extension_files(const String &p_extension_directory, const String &p_extra_class = String());

	// Removes a class from the manifest and its sources, then regenerates.
	static Error remove_class(const String &p_extension_directory, const String &p_class_name);

	// Handles the command line used for headless testing.
	static void run_from_command_line();

	static String class_name_for(const Ref<GDScript> &p_script);
	// `class_name` to suggest for a script that has none (sanitized PascalCase of
	// its file name).
	static String suggested_class_name(const String &p_script_path);
	static String snake_case(const String &p_name);
	static String class_file_stem(const String &p_class_name);
	static LocalVector<GritGdxSkipped> collect_skipped(const GritGdxClass &p_class);
	// Records a script and classifies its methods without writing any file: the
	// pre-flight plan (and the CLI's --grit-gdx-plan) builds on it.
	static Error collect(const String &p_script_path, const String &p_extension_directory, GritGdxClass &r_class, String &r_error);

private:
	static Error write_file(const String &p_path, const String &p_content);
	static Error write_registry(const String &p_extension_directory, const GritGdxManifest &p_manifest);
	static String pascal_case(const String &p_text);
	static String sanitize_identifier(const String &p_name);
};
