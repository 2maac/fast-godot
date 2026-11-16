#pragma once

#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// One class inside a generated extension.
struct GritGdxManifestEntry {
	String class_name;
	String script_path; // res:// path of the source script
	String base_type; // native base class the node had before conversion
	String archived_script; // res:// path of the .gde archive, when the script was archived
	bool is_tool = false;
};

// Bookkeeping for a generated extension folder: which classes it contains, which
// scripts they came from, and what the node has to be restored to on revert.
class GritGdxManifest {
public:
	static constexpr const char *FILE_NAME = ".grit_manifest.json";

	LocalVector<GritGdxManifestEntry> classes;

	Error load(const String &p_extension_directory);
	Error save(const String &p_extension_directory) const;

	const GritGdxManifestEntry *find_class(const String &p_class_name) const;
	const GritGdxManifestEntry *find_script(const String &p_script_path) const;
	bool has_class(const String &p_class_name) const;
	void remove_class(const String &p_class_name);
	// Adds or replaces the entry for the same script.
	void upsert(const GritGdxManifestEntry &p_entry);

	// Extension name = folder name; it is also the library and entry-symbol name.
	static String extension_name(const String &p_extension_directory);
	// Folders under res://gdext that contain a generated extension.
	static Vector<String> list_extensions();
};
