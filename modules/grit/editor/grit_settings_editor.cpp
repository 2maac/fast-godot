#include "grit_settings_editor.h"

#include "modules/grit/editor/grit_provisioner.h"

#include "core/object/callable_mp.h"

#include "editor/editor_node.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/button.h"
#include "scene/gui/box_container.h"
#include "scene/gui/line_edit.h"

// One row: the path, a button to pick an existing folder, and a button to download
// the source into one. All three write the same setting.
GritSourcePathEditor::GritSourcePathEditor() {
	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);

	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_placeholder(TTR("Folder containing the Fast_Godot source"));
	path_edit->connect("text_submitted", callable_mp(this, &GritSourcePathEditor::_text_submitted));
	path_edit->connect("text_changed", callable_mp(this, &GritSourcePathEditor::_text_changed));
	row->add_child(path_edit);

	browse_button = memnew(Button);
	browse_button->set_text(TTR("..."));
	browse_button->set_tooltip_text(TTR("Choose an existing Fast_Godot source folder."));
	browse_button->connect("pressed", callable_mp(this, &GritSourcePathEditor::_browse_pressed));
	row->add_child(browse_button);

	fetch_button = memnew(Button);
	fetch_button->set_text(TTR("Download"));
	fetch_button->set_tooltip_text(TTR("Download this editor's Fast_Godot source into a folder."));
	fetch_button->connect("pressed", callable_mp(this, &GritSourcePathEditor::_fetch_pressed));
	row->add_child(fetch_button);

	// Where to download to.
	dest_dialog = memnew(EditorFileDialog);
	dest_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	dest_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	dest_dialog->set_title(TTR("Choose a folder for the Fast_Godot source"));
	dest_dialog->connect("dir_selected", callable_mp(this, &GritSourcePathEditor::_dest_chosen));
	add_child(dest_dialog);

	// Point at an existing checkout.
	browse_dialog = memnew(EditorFileDialog);
	browse_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	browse_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	browse_dialog->set_title(TTR("Choose the Fast_Godot source folder"));
	browse_dialog->connect("dir_selected", callable_mp(this, &GritSourcePathEditor::_browse_chosen));
	add_child(browse_dialog);
}

void GritSourcePathEditor::update_property() {
	if (path_edit == nullptr) {
		return;
	}
	const String value = String(get_edited_object()->get(get_edited_property()));
	if (path_edit->get_text() != value) {
		path_edit->set_text(value);
	}
}

void GritSourcePathEditor::_text_changed(const String &p_text) {
	// Typing should not mark the setting dirty on every keystroke, but the value has
	// to be current when the user leaves the field.
	if (path_edit && p_text == path_edit->get_text()) {
		return;
	}
}

void GritSourcePathEditor::_text_submitted(const String &p_text) {
	emit_changed(get_edited_property(), p_text.strip_edges());
}

void GritSourcePathEditor::_browse_pressed() {
	browse_dialog->popup_centered_ratio(0.5);
}

void GritSourcePathEditor::_fetch_pressed() {
	dest_dialog->popup_centered_ratio(0.5);
}

void GritSourcePathEditor::_browse_chosen(const String &p_dir) {
	path_edit->set_text(p_dir);
	emit_changed(get_edited_property(), p_dir);
}

void GritSourcePathEditor::_dest_chosen(const String &p_dir) {
	pending_dest = p_dir;
	String error;
	// Downloads the source archive for this editor's commit, unpacks it into the
	// chosen folder and deletes the archive; the path entry receives the tree.
	const String tree = GritProvisioner::fetch_source_to(pending_dest, true, &error);
	if (tree.is_empty()) {
		ERR_PRINT("Grit: " + error);
		return;
	}
	path_edit->set_text(tree);
	emit_changed(get_edited_property(), tree);
}

bool GritSettingsPlugin::can_handle(Object *p_object) {
	// The settings dialog edits a SectionedInspectorFilter rather than EditorSettings
	// itself, so the object cannot be used to decide. parse_property matches on the
	// property path alone, which is the only thing this plugin cares about.
	return true;
}

bool GritSettingsPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	// The settings dialog hands editors a path relative to the selected section
	// ("source_path"), not the full setting name, so match either form.
	const bool ours = p_path == GritProvisioner::SETTING_SOURCE_PATH ||
			p_path == "source_path" ||
			p_path.ends_with("/source_path");
	if (ours) {
		add_property_editor(p_path, memnew(GritSourcePathEditor));
		return true;
	}
	return false;
}
