#pragma once

#include "export_plugin.h"

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
};
