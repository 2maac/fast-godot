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
	return p_extension_directory.ends_with("/") ? p_extension_directory.left(-1).get_file() : p_extension_directory.get_file();
}

Vector<String> GritGdxManifest::list_extensions() {
	Vector<String> extensions;
	Ref<DirAccess> root = DirAccess::open("res://gdext");
	if (root.is_null()) {
		return extensions;
	}
	for (const String &name : root->get_directories()) {
		const String directory = "res://gdext/" + name;
		if (FileAccess::exists(directory.path_join(FILE_NAME))) {
			extensions.push_back(directory);
		}
	}
	extensions.sort();
	return extensions;
}
