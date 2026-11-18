#include "grit_gdx_script.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#ifdef TOOLS_ENABLED
#include "modules/grit/gdextension/grit_gdx_manifest.h"
#endif

void GritGdxScript::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_archived_script_stale"), &GritGdxScript::is_stale);
	ClassDB::bind_method(D_METHOD("get_archived_base_type"), &GritGdxScript::get_archived_base_type);
	ClassDB::bind_method(D_METHOD("get_archived_class"), &GritGdxScript::get_archived_class);
}

// The library recorded the archive's hash when it was built; anything else means
// the archive was edited since and the class is out of date.
bool GritGdxScript::is_stale() const {
#ifdef TOOLS_ENABLED
	const String path = get_path();
	if (path.is_empty() || !FileAccess::exists(path)) {
		return false;
	}
	String built;
	for (const String &directory : GritGdxManifest::list_extensions()) {
		GritGdxManifest manifest;
		if (manifest.load(directory) != OK) {
			continue;
		}
		if (const GritGdxManifestEntry *entry = manifest.find_archived(path)) {
			built = entry->source_hash;
			break;
		}
	}
	if (built.is_empty()) {
		return false;
	}
	Error error = OK;
	const String source = FileAccess::get_file_as_string(path, &error);
	return error == OK && source.sha256_text() != built;
#else
	return false;
#endif
}

#ifdef TOOLS_ENABLED
StringName GritGdxScript::get_editor_icon() const {
	return is_stale() ? SNAME("GritGdxScriptModified") : SNAME("GritGdxScript");
}
#endif

void GritGdxScript::set_archived_metadata(const StringName &p_base_type, const String &p_class) {
	archived_base_type = p_base_type;
	archived_class = p_class;
}

// `extends Node2D` is the only form the converter accepts (a script parent is
// refused), so the first one names the native base type of the archived class.
static StringName grit_gdx_archived_base_type(const String &p_source) {
	for (const String &line : p_source.split("\n")) {
		const String stripped = line.strip_edges();
		if (!stripped.begins_with("extends ")) {
			continue;
		}
		String base = stripped.substr(8).strip_edges();
		if (base.is_empty() || base.begins_with("\"") || base.begins_with("'")) {
			// Inheriting another script: the converter refuses those, so there is no
			// native base type to report.
			return StringName();
		}
		const int comment = base.find("#");
		if (comment >= 0) {
			base = base.substr(0, comment).strip_edges();
		}
		return StringName(base);
	}
	return StringName();
}

Ref<Resource> GritGdxScriptLoader::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	const String path = p_original_path.is_empty() ? p_path : p_original_path;
	Error error = OK;
	const String source = FileAccess::get_file_as_string(path, &error);
	if (r_error) {
		*r_error = error;
	}
	if (error != OK) {
		return Ref<Resource>();
	}

	Ref<GritGdxScript> script;
	script.instantiate();
	// The path is assigned by ResourceLoader: setting it here makes a second
	// instance (ignore/replace cache modes) fight the cached one over the path.
	script->set_name(path.get_file());
	script->set_source_code(source);
	script->set_archived_metadata(grit_gdx_archived_base_type(source), String());
	return script;
}

void GritGdxScriptLoader::get_recognized_extensions(List<String> *p_extensions) const {
	// `.gds` is the archived source of a converted class; `.gde` is the legacy
	// archive that used to stay attached to the node.
	p_extensions->push_back("gds");
	p_extensions->push_back("gde");
}

bool GritGdxScriptLoader::handles_type(const String &p_type) const {
	return p_type == "Script" || p_type == "GritGdxScript";
}

String GritGdxScriptLoader::get_resource_type(const String &p_path) const {
	const String extension = p_path.get_extension().to_lower();
	return (extension == "gds" || extension == "gde") ? String("GritGdxScript") : String();
}

Error GritGdxScriptSaver::save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	Ref<GritGdxScript> script = p_resource;
	if (script.is_null()) {
		return ERR_INVALID_PARAMETER;
	}
	Error error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &error);
	if (file.is_null()) {
		return error;
	}
	file->store_string(script->get_source_code());
	return OK;
}

void GritGdxScriptSaver::get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const {
	if (p_resource.is_valid() && p_resource->is_class("GritGdxScript")) {
		p_extensions->push_back("gds");
		p_extensions->push_back("gde");
	}
}

bool GritGdxScriptSaver::recognize(const Ref<Resource> &p_resource) const {
	return p_resource.is_valid() && p_resource->is_class("GritGdxScript");
}
