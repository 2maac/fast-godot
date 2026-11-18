#pragma once

#include "grit_gdx_apply.h"

#include "editor/inspector/editor_context_menu_plugin.h"

class Node;
class AcceptDialog;
class ConfirmationDialog;
class Label;
class LineEdit;
class PopupMenu;
class ProgressBar;
class Timer;
class GritGdxJob;

// Scene tree actions: make a scripted node a generated class (new extension or
// an existing one) and convert a generated node back to its script.
class GritGdxContextMenuPlugin : public EditorContextMenuPlugin {
public:
	~GritGdxContextMenuPlugin();

	virtual void get_options(const Vector<String> &p_paths) override;

	static bool can_make_gdextension(Node *p_node);
	static void run_menu_check(const String &p_scene_path);
	// Debug harness (GRIT_GDX_AUTOTEST=<scene>): drives the menu's entry points in
	// a real editor window and reports what the tree shows at each step.
	void run_autotest(const String &p_scene_path);

private:
	void _make_gdextension(const Array &p_selection);
	void _add_to_extension(const Array &p_selection);
	void _convert_back(const Array &p_selection);
	void _rebuild(const Array &p_selection);
	static void _show_message(const String &p_text);
	void _submenu_selected(int p_index);

	void _prompt_extension_name(const String &p_script_path, const String &p_scene_path, const String &p_node_path, const String &p_default_name);
	void _start_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path, bool p_allow_partial);
	// A partial conversion needs an explicit "Convert Anyway".
	void _confirm_partial(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path, const GritGdxApply::Plan &p_plan);
	void _confirm_partial_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path);
	void _start_revert(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path, bool p_replace_script = false);
	// Confirms deleting a `.gd` that was added after the conversion.
	void _confirm_replace(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path, const String &p_script_path, const String &p_archive_path);
	void _start_job();
	void _poll();
	void _close_dialog();

	Ref<GritGdxJob> job;
	AcceptDialog *dialog = nullptr;
	ConfirmationDialog *name_dialog = nullptr;
	ConfirmationDialog *confirm_dialog = nullptr;
	LineEdit *name_edit = nullptr;
	Label *status_label = nullptr;
	ProgressBar *progress_bar = nullptr;
	Timer *timer = nullptr;
	PopupMenu *submenu = nullptr;
	Vector<String> submenu_extensions;
	String active_scene;
	Timer *autotest_timer = nullptr;
	int autotest_step = 0;
	bool autotest_opened = false;
	bool action_finished = true;
	String autotest_scene;
	void _autotest_tick();
};
