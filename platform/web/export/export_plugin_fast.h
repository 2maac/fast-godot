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
};
