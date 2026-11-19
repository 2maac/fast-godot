#include "grit_settings_editor.h"

#include "modules/grit/editor/grit_export_plugin.h"
#include "modules/grit/editor/grit_provisioner.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/file_system/editor_paths.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/settings/editor_settings.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/line_edit.h"

GritPathEditor::GritPathEditor(const String &p_label) {
	set_label(p_label);

	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);

	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_placeholder(TTR("Folder containing the Fast_Godot toolchain"));
	path_edit->connect("text_submitted", callable_mp(this, &GritPathEditor::_text_submitted));
	path_edit->connect("text_changed", callable_mp(this, &GritPathEditor::_text_changed));
	path_edit->connect("focus_exited", callable_mp(this, &GritPathEditor::_focus_exited));
	row->add_child(path_edit);

	browse_button = memnew(Button);
	browse_button->set_text(TTR("..."));
	browse_button->set_tooltip_text(TTR("Choose an existing folder."));
	browse_button->connect("pressed", callable_mp(this, &GritPathEditor::_browse_pressed));
	row->add_child(browse_button);

	browse_dialog = memnew(EditorFileDialog);
	browse_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	browse_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	browse_dialog->set_title(TTR("Choose the toolchain folder"));
	browse_dialog->connect("dir_selected", callable_mp(this, &GritPathEditor::_browse_chosen));
	add_child(browse_dialog);
}

void GritPathEditor::update_property() {
	if (path_edit == nullptr) {
		return;
	}
	const String value = String(get_edited_object()->get(get_edited_property()));
	if (path_edit->get_text() != value) {
		path_edit->set_text(value);
	}
}

void GritPathEditor::_text_changed(const String &p_text) {
	// Commit as the text changes: a pasted path must take effect without the user
	// knowing to press Enter, which is what the first version required.
	emit_changed(get_edited_property(), p_text);
}

void GritPathEditor::_focus_exited() {
	emit_changed(get_edited_property(), path_edit->get_text().strip_edges());
}

void GritPathEditor::_text_submitted(const String &p_text) {
	emit_changed(get_edited_property(), p_text.strip_edges());
}

void GritPathEditor::_browse_pressed() {
	browse_dialog->popup_centered_ratio(0.5);
}

void GritPathEditor::_browse_chosen(const String &p_dir) {
	path_edit->set_text(p_dir);
	emit_changed(get_edited_property(), p_dir);
}

GritBuildProfileEditor::GritBuildProfileEditor() {
	HBoxContainer *row = memnew(HBoxContainer);
	add_child(row);

	path_edit = memnew(LineEdit);
	path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	path_edit->set_placeholder(TTR("Optional engine compilation profile (.build/.gdbuild); empty builds the full engine"));
	path_edit->connect("text_submitted", callable_mp(this, &GritBuildProfileEditor::_text_submitted));
	path_edit->connect("text_changed", callable_mp(this, &GritBuildProfileEditor::_text_changed));
	path_edit->connect("focus_exited", callable_mp(this, &GritBuildProfileEditor::_focus_exited));
	row->add_child(path_edit);

	browse_button = memnew(Button);
	browse_button->set_text(TTR("..."));
	browse_button->set_tooltip_text(TTR("Choose a build profile file."));
	browse_button->connect("pressed", callable_mp(this, &GritBuildProfileEditor::_browse_pressed));
	row->add_child(browse_button);

	browse_dialog = memnew(EditorFileDialog);
	browse_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_FILE);
	browse_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	browse_dialog->set_title(TTR("Choose an engine compilation profile"));
	browse_dialog->add_filter("*.gdbuild,*.build", TTR("Engine Compilation Profile"));
	browse_dialog->set_current_dir(ProjectSettings::get_singleton()->globalize_path("res://"));
	browse_dialog->connect("file_selected", callable_mp(this, &GritBuildProfileEditor::_browse_chosen));
	add_child(browse_dialog);
}

void GritBuildProfileEditor::update_property() {
	if (path_edit == nullptr) {
		return;
	}
	const String value = String(get_edited_object()->get(get_edited_property()));
	if (path_edit->get_text() != value) {
		path_edit->set_text(value);
	}
}

void GritBuildProfileEditor::_text_changed(const String &p_text) {
	emit_changed(get_edited_property(), p_text);
}

void GritBuildProfileEditor::_focus_exited() {
	emit_changed(get_edited_property(), path_edit->get_text().strip_edges());
}

void GritBuildProfileEditor::_text_submitted(const String &p_text) {
	emit_changed(get_edited_property(), p_text.strip_edges());
}

void GritBuildProfileEditor::_browse_pressed() {
	browse_dialog->popup_centered_ratio(0.5);
}

void GritBuildProfileEditor::_browse_chosen(const String &p_path) {
	const String project_root = ProjectSettings::get_singleton()->globalize_path("res://").simplify_path().trim_suffix("/");
	String chosen = p_path.simplify_path();
	if (chosen.begins_with(project_root + "/")) {
		chosen = "res://" + chosen.substr(project_root.length() + 1);
	}
	path_edit->set_text(chosen);
	emit_changed(get_edited_property(), chosen);
}

bool GritSettingsPlugin::can_handle(Object *p_object) {
	// The settings dialog edits a SectionedInspectorFilter rather than EditorSettings
	// itself, so the object cannot be used to decide. parse_property matches on the
	// property path alone, which is the only thing this plugin cares about.
	return true;
}

bool GritSettingsPlugin::parse_property(Object *p_object, const Variant::Type p_type, const String &p_path, const PropertyHint p_hint, const String &p_hint_text, const BitField<PropertyUsageFlags> p_usage, const bool p_wide) {
	// The settings dialog hands editors a path relative to the selected section, so
	// match the full name or the trailing segment of it.
	if (p_path == GritExportPlugin::SETTING_BUILD_PROFILE || p_path.ends_with("/build_profile")) {
		add_property_editor(p_path, memnew(GritBuildProfileEditor), false, TTR("Build Profile"));
		return true;
	}
	const bool toolchain = p_path == GritProvisioner::SETTING_TOOLCHAIN_ROOT ||
			p_path == "toolchain_root_folder_path" ||
			p_path.ends_with("/toolchain_root_folder_path");
	if (toolchain) {
		add_property_editor(p_path, memnew(GritPathEditor(TTR("ToolChain Root Folder Path"))), false, TTR("ToolChain Root Folder Path"));
		return true;
	}
	return false;
}
