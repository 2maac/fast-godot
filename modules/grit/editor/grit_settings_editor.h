#pragma once

#include "editor/inspector/editor_inspector.h"

class Button;
class EditorFileDialog;
class LineEdit;

// Editor Settings > Grit > Settings has one row, and it needs to do two things that
// a plain setting cannot: offer a "Fetch Source" button that downloads this editor's
// engine source into a folder the user picks, and show the resulting path in an
// entry the user can also type into. Both are drawn by GritSourcePathEditor, which
// is installed for that one property by GritSettingsPlugin.
class GritSourcePathEditor : public EditorProperty {
	GDCLASS(GritSourcePathEditor, EditorProperty);

	Button *fetch_button = nullptr;
	LineEdit *path_edit = nullptr;
	Button *browse_button = nullptr;
	EditorFileDialog *browse_dialog = nullptr;
	EditorFileDialog *dest_dialog = nullptr;
	String pending_dest;

	void _browse_pressed();
	void _fetch_pressed();
	void _dest_chosen(const String &p_dir);
	void _browse_chosen(const String &p_dir);
	void _text_submitted(const String &p_text);
	void _text_changed(const String &p_text);

protected:
	void update_property() override;

public:
	GritSourcePathEditor();
};

class GritSettingsPlugin : public EditorInspectorPlugin {
	GDCLASS(GritSettingsPlugin, EditorInspectorPlugin);

public:
	virtual bool can_handle(Object *p_object) override;
	virtual bool parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) override;
};
