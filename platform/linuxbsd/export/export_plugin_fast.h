#pragma once

#include "export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

// "Linux Fast": compiles the project's scripts into a per-project template instead
// of shipping GDScript. The template files carry a _fast suffix so a Fast export
// can never pick up, or overwrite, the stock template.
class EditorExportPlatformLinuxFast : public EditorExportPlatformLinuxBSD {
	GDCLASS(EditorExportPlatformLinuxFast, EditorExportPlatformLinuxBSD);

public:
	virtual String get_name() const override {
		return "Linux Fast";
	}
	virtual String get_template_file_name(const String &p_target, const String &p_arch) const override {
		return "linux_fast_" + p_target + "." + p_arch;
	}

	// The Fast template belongs to the project (<project>/exports), not to the
	// editor's template directory.
	static String template_dir() {
		return ProjectSettings::get_singleton()->globalize_path("res://exports");
	}
	virtual String find_export_template(const String &p_template_file_name, String *r_err = nullptr) const override {
		const String path = template_dir().path_join(p_template_file_name);
		if (FileAccess::exists(path)) {
			return path;
		}
		if (r_err) {
			*r_err += TTR("No export template found at the expected path:") + "\n" + path + "\n";
		}
		return String();
	}
	virtual bool exists_export_template(const String &p_template_file_name, String *r_err) const override {
		return FileAccess::exists(template_dir().path_join(p_template_file_name));
	}
};
