#pragma once

#include "export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

// "HTML5 Fast": the project's Grit-compiled code in a WebAssembly template. Unlike
// the native Fast platforms this one is registered on every host, because Emscripten
// cross-compiles to wasm from macOS, Windows and Linux alike.
class EditorExportPlatformWebFast : public EditorExportPlatformWeb {
	GDCLASS(EditorExportPlatformWebFast, EditorExportPlatformWeb);

public:
	virtual String get_name() const override {
		return "HTML5 Fast";
	}

	// The compiled template belongs to the project, never to the editor's template
	// directory, and presets therefore need no custom_template path.
	static constexpr const char *TEMPLATE_FILE = "web_fast.zip";

	static constexpr const char *TEMPLATE_DIR = "res://exports";

	static String template_path() {
		return ProjectSettings::get_singleton()->globalize_path(String(TEMPLATE_DIR) + "/" + TEMPLATE_FILE);
	}

	virtual String find_export_template(const String &p_template_file_name, String *r_err = nullptr) const override {
		const String path = template_path();
		if (FileAccess::exists(path)) {
			return path;
		}
		if (r_err) {
			*r_err += TTR("No export template found at the expected path:") + "\n" + path + "\n";
		}
		return String();
	}
	virtual bool exists_export_template(const String &p_template_file_name, String *r_err) const override {
		return FileAccess::exists(template_path());
	}

	// The template is produced by the export itself, so "no template yet" must not
	// disable the Export button. Any other configuration error still does.
	// (The WebGPU-specific renderer check below is the only addition to the
	// pre-WebGPU Fast platform.)
	virtual bool has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug) const override {
		const String rendering_method = String(get_project_setting(p_preset, "rendering/renderer/rendering_method.web"));
		const bool webgpu_method = rendering_method == "mobile";
		String message;
		bool missing = false;
		const bool base_valid = EditorExportPlatformWeb::has_valid_export_configuration(p_preset, message, missing, p_debug);
		if (base_valid) {
			if (!webgpu_method) {
				r_error = vformat("The HTML5 Fast template only contains the WebGPU Forward Mobile renderer; set rendering/renderer/rendering_method.web to \"mobile\" (currently \"%s\").", rendering_method);
				r_missing_templates = false;
				return false;
			}
			r_error = message;
			r_missing_templates = missing;
			return true;
		}
		if (!missing) {
			r_error = message;
			r_missing_templates = missing;
			return false;
		}
		// Drop the template lines, keep whatever else is wrong.
		Vector<String> kept;
		for (const String &line : message.split("\n")) {
			if (!line.begins_with("No export template found")) {
				kept.push_back(line);
			}
		}
		if (!webgpu_method) {
			kept.push_back(vformat("The HTML5 Fast template only contains the WebGPU Forward Mobile renderer; set rendering/renderer/rendering_method.web to \"mobile\" (currently \"%s\").", rendering_method));
		}
		r_error = String("\n").join(kept).strip_edges();
		r_missing_templates = false;
		return r_error.is_empty();
	}

	// `has_valid_export_configuration` is only consulted by the editor UI; a
	// scripted export (`--export-release "HTML5 Fast" ...`) goes straight to
	// export_project, so enforce the renderer check here as well.
	virtual Error export_project(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path, BitField<EditorExportPlatform::DebugFlags> p_flags = 0, bool p_notify = true) override {
		const String rendering_method = String(get_project_setting(p_preset, "rendering/renderer/rendering_method.web"));
		if (rendering_method != "mobile") {
			ERR_PRINT(vformat("The HTML5 Fast template only contains the WebGPU Forward Mobile renderer; set rendering/renderer/rendering_method.web to \"mobile\" (currently \"%s\").", rendering_method));
			return ERR_UNCONFIGURED;
		}
		return EditorExportPlatformWeb::export_project(p_preset, p_debug, p_path, p_flags, p_notify);
	}
};
