#include "grit_gdx_script.h"

#include "core/io/file_access.h"
#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

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
	script->set_path(path);
	script->set_name(path.get_file());
	script->set_source_code(source);
	script->set_archived_metadata(grit_gdx_archived_base_type(source), String());
	return script;
}

void GritGdxScriptLoader::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gde");
}

bool GritGdxScriptLoader::handles_type(const String &p_type) const {
	return p_type == "Script" || p_type == "GritGdxScript";
}

String GritGdxScriptLoader::get_resource_type(const String &p_path) const {
	return p_path.get_extension().to_lower() == "gde" ? String("GritGdxScript") : String();
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
		p_extensions->push_back("gde");
	}
}

bool GritGdxScriptSaver::recognize(const Ref<Resource> &p_resource) const {
	return p_resource.is_valid() && p_resource->is_class("GritGdxScript");
}
