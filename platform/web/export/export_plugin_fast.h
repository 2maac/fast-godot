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
	virtual bool has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug) const override {
		String message;
		bool missing = false;
		const bool base_valid = EditorExportPlatformWeb::has_valid_export_configuration(p_preset, message, missing, p_debug);
		if (!base_valid && !missing) {
			r_error = message;
			r_missing_templates = false;
			return false;
		}
		// Drop the template lines, keep whatever else is wrong.
		Vector<String> kept;
		for (const String &line : message.split("\n")) {
			if (!line.begins_with("No export template found")) {
				kept.push_back(line);
			}
		}
		// The wasm template only contains the WebGPU rendering device (the builds are
		// made with webgpu=yes opengl3=no), so the project has to select the mobile or
		// forward_plus rendering method for web. Without it the engine tries to create
		// the GLES3 renderer, which is not compiled into the template, and the page
		// never boots. Catch that here instead of in the browser console.
		const String rendering_method = String(get_project_setting(p_preset, "rendering/renderer/rendering_method.web"));
		const bool webgpu_method = rendering_method == "mobile" || rendering_method == "forward_plus";
		if (!webgpu_method) {
			kept.push_back(TTR("The HTML5 Fast template only contains the WebGPU renderer. Set \"rendering/renderer/rendering_method.web\" to \"mobile\" (or \"forward_plus\") in Project Settings > Rendering > Renderer, and make sure \"rendering/rendering_device/driver.web\" is \"webgpu\"."));
		}
		r_error = String("\n").join(kept).strip_edges();
		r_missing_templates = false;
		return base_valid && webgpu_method;
	}
};
