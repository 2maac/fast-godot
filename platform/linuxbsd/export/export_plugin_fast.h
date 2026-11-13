#pragma once

#include "export_plugin.h"

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
};
