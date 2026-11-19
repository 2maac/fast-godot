#pragma once

#include "editor/inspector/editor_inspector.h"

class Button;
class EditorFileDialog;
class LineEdit;

// A settings row that is a path plus a button to pick an existing folder.
class GritPathEditor : public EditorProperty {
	GDCLASS(GritPathEditor, EditorProperty);

	LineEdit *path_edit = nullptr;
	Button *browse_button = nullptr;
	EditorFileDialog *browse_dialog = nullptr;

	void _browse_pressed();
	void _browse_chosen(const String &p_dir);
	void _text_submitted(const String &p_text);
	void _text_changed(const String &p_text);
	void _focus_exited();

protected:
	void update_property() override;

public:
	GritPathEditor(const String &p_label);
};

// A project settings row for the build profile file: editable path plus a browse
// button that picks an existing .json profile.
class GritBuildProfileEditor : public EditorProperty {
	GDCLASS(GritBuildProfileEditor, EditorProperty);

	LineEdit *path_edit = nullptr;
	Button *browse_button = nullptr;
	EditorFileDialog *browse_dialog = nullptr;

	void _browse_pressed();
	void _browse_chosen(const String &p_path);
	void _text_submitted(const String &p_text);
	void _text_changed(const String &p_text);
	void _focus_exited();

protected:
	void update_property() override;

public:
	GritBuildProfileEditor();
};

class GritSettingsPlugin : public EditorInspectorPlugin {
	GDCLASS(GritSettingsPlugin, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override;
};
