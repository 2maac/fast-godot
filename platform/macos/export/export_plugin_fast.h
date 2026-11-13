#pragma once

#include "export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

// A second macOS export platform that builds the project's scripts into the
// export instead of shipping them as GDScript. It is the same platform in every
// other respect: choosing it in the export settings is what selects "compiled"
// for that preset, so there is no editor-wide mode to remember.
class EditorExportPlatformMacOSFast : public EditorExportPlatformMacOS {
	GDCLASS(EditorExportPlatformMacOSFast, EditorExportPlatformMacOS);

public:
	virtual String get_name() const override {
		return "macOS Fast";
	}

	// The compiled template belongs to the project, not to the editor: it lives in
	// <project>/exports/ so it travels with the project and never collides with the
	// stock macos.zip that every other project shares. Presets therefore never need
	// a custom_template path.
	static constexpr const char *TEMPLATE_FILE = "macos_fast.zip";

	static constexpr const char *TEMPLATE_DIR = "res://exports";

	// Absolute path of this project's compiled template.
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
