#pragma once

#include "editor/inspector/editor_inspector.h"

class Button;
class EditorFileDialog;
class LineEdit;

// A settings row that is a path plus the buttons to use it: pick an existing folder,
// or download the thing into one. The same row serves both the source and the
// toolchain; the mode decides what the download fetches.
class GritPathEditor : public EditorProperty {
	GDCLASS(GritPathEditor, EditorProperty);

	bool toolchain = false;
	LineEdit *path_edit = nullptr;
	Button *browse_button = nullptr;
	Button *download_button = nullptr;
	EditorFileDialog *browse_dialog = nullptr;
	EditorFileDialog *dest_dialog = nullptr;

	void _browse_pressed();
	void _download_pressed();
	void _dest_chosen(const String &p_dir);
	void _browse_chosen(const String &p_dir);
	void _text_submitted(const String &p_text);
	void _text_changed(const String &p_text);

protected:
	void update_property() override;

public:
	GritPathEditor(bool p_toolchain, const String &p_label);
};

class GritSettingsPlugin : public EditorInspectorPlugin {
	GDCLASS(GritSettingsPlugin, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override;
};
