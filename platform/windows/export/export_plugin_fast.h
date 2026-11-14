#pragma once

#include "export_plugin.h"

#include "core/config/project_settings.h"
#include "core/io/file_access.h"

// "Windows Fast": compiles the project's scripts into a per-project template. The
// _fast suffix keeps it apart from the stock windows_* templates.
class EditorExportPlatformWindowsFast : public EditorExportPlatformWindows {
	GDCLASS(EditorExportPlatformWindowsFast, EditorExportPlatformWindows);

public:
	virtual String get_name() const override {
		return "Windows Fast";
	}
	virtual String get_template_file_name(const String &p_target, const String &p_arch) const override {
		return "windows_fast_" + p_target + "_" + p_arch + ".exe";
	}

	// The Fast template belongs to the project (<project>/exports), so presets never
	// carry a custom_template path and the stock templates are never touched.
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

	// The template is produced by the export itself, so "no template yet" must not
	// disable the Export button. Any other configuration error still does.
	virtual bool has_valid_export_configuration(const Ref<EditorExportPreset> &p_preset, String &r_error, bool &r_missing_templates, bool p_debug) const override {
		String message;
		bool missing = false;
		const bool base_valid = EditorExportPlatformWindows::has_valid_export_configuration(p_preset, message, missing, p_debug);
		if (base_valid) {
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
		r_error = String("\n").join(kept).strip_edges();
		r_missing_templates = false;
		return r_error.is_empty();
	}
};
