#include "grit_gdx_manifest.h"

#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"

Error GritGdxManifest::load(const String &p_extension_directory) {
	classes.clear();
	const String path = p_extension_directory.path_join(FILE_NAME);
	if (!FileAccess::exists(path)) {
		return OK;
	}
	Error error = OK;
	const String text = FileAccess::get_file_as_string(path, &error);
	if (error != OK) {
		return error;
	}
	Ref<JSON> json;
	json.instantiate();
	error = json->parse(text);
	if (error != OK) {
		return error;
	}
	const Dictionary root = json->get_data();
	const Array list = root.get("classes", Array());
	for (const Variant &item : list) {
		const Dictionary entry = item;
		if (entry.is_empty()) {
			continue;
		}
		GritGdxManifestEntry parsed;
		parsed.class_name = entry.get("class", String());
		parsed.script_path = entry.get("script", String());
		parsed.base_type = entry.get("base", String());
		parsed.archived_script = entry.get("archived", String());
		parsed.source_hash = entry.get("source_hash", String());
		parsed.is_tool = entry.get("tool", false);
		if (!parsed.class_name.is_empty()) {
			classes.push_back(parsed);
		}
	}
	return OK;
}

Error GritGdxManifest::save(const String &p_extension_directory) const {
	Error error = DirAccess::make_dir_recursive_absolute(p_extension_directory);
	ERR_FAIL_COND_V_MSG(error != OK, error, vformat("Grit: could not create \"%s\".", p_extension_directory));

	Array list;
	for (const GritGdxManifestEntry &entry : classes) {
		Dictionary item;
		item["class"] = entry.class_name;
		item["script"] = entry.script_path;
		item["base"] = entry.base_type;
		if (!entry.archived_script.is_empty()) {
			item["archived"] = entry.archived_script;
		}
		if (!entry.source_hash.is_empty()) {
			item["source_hash"] = entry.source_hash;
		}
		if (entry.is_tool) {
			item["tool"] = true;
		}
		list.push_back(item);
	}
	Dictionary root;
	root["version"] = 1;
	root["classes"] = list;

	Ref<FileAccess> file = FileAccess::open(p_extension_directory.path_join(FILE_NAME), FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_CREATE, vformat("Grit: could not write \"%s\".", p_extension_directory.path_join(FILE_NAME)));
	file->store_string(JSON::stringify(root, "\t") + "\n");
	return OK;
}

const GritGdxManifestEntry *GritGdxManifest::find_archived(const String &p_archived_path) const {
	for (const GritGdxManifestEntry &entry : classes) {
		if (!entry.archived_script.is_empty() && entry.archived_script == p_archived_path) {
			return &entry;
		}
	}
	return nullptr;
}

const GritGdxManifestEntry *GritGdxManifest::find_class(const String &p_class_name) const {
	for (const GritGdxManifestEntry &entry : classes) {
		if (entry.class_name == p_class_name) {
			return &entry;
		}
	}
	return nullptr;
}

const GritGdxManifestEntry *GritGdxManifest::find_script(const String &p_script_path) const {
	for (const GritGdxManifestEntry &entry : classes) {
		if (entry.script_path == p_script_path) {
			return &entry;
		}
	}
	return nullptr;
}

bool GritGdxManifest::has_class(const String &p_class_name) const {
	return find_class(p_class_name) != nullptr;
}

void GritGdxManifest::remove_class(const String &p_class_name) {
	for (uint32_t i = 0; i < classes.size(); i++) {
		if (classes[i].class_name == p_class_name) {
			classes.remove_at(i);
			return;
		}
	}
}

void GritGdxManifest::upsert(const GritGdxManifestEntry &p_entry) {
	for (GritGdxManifestEntry &entry : classes) {
		if (entry.script_path == p_entry.script_path || entry.class_name == p_entry.class_name) {
			entry = p_entry;
			return;
		}
	}
	classes.push_back(p_entry);
}

String GritGdxManifest::extension_name(const String &p_extension_directory) {
	String name = p_extension_directory.ends_with("/") ? p_extension_directory.left(-1).get_file() : p_extension_directory.get_file();
	// Extensions created from a `.gde` source live in `<stem>_gdext/` and are
	// named after the class (`<stem>.gdextension`).
	if (name.ends_with("_gdext")) {
		name = name.left(-6);
	}
	return name;
}

static void _scan_extensions(const String &p_directory, Vector<String> &r_extensions) {
	// An extension directory owns a manifest; do not descend into it.
	if (FileAccess::exists(p_directory.path_join(".grit_manifest.json"))) {
		r_extensions.push_back(p_directory);
		return;
	}
	for (const String &directory : DirAccess::get_directories_at(p_directory)) {
		if (directory.begins_with(".")) {
			continue;
		}
		_scan_extensions(p_directory.path_join(directory), r_extensions);
	}
}

Vector<String> GritGdxManifest::list_extensions() {
	Vector<String> extensions;
	_scan_extensions("res://", extensions);
	extensions.sort();
	return extensions;
}

