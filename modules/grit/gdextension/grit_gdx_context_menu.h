#pragma once

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

private:
	void _make_gdextension(const Array &p_selection);
	void _add_to_extension(const Array &p_selection);
	void _convert_back(const Array &p_selection);
	void _submenu_selected(int p_index);

	void _prompt_extension_name(const String &p_script_path, const String &p_scene_path, const String &p_node_path, const String &p_default_name);
	void _start_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path);
	void _start_revert(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path);
	void _start_job();
	void _poll();
	void _close_dialog();

	Ref<GritGdxJob> job;
	AcceptDialog *dialog = nullptr;
	ConfirmationDialog *name_dialog = nullptr;
	LineEdit *name_edit = nullptr;
	Label *status_label = nullptr;
	ProgressBar *progress_bar = nullptr;
	Timer *timer = nullptr;
	PopupMenu *submenu = nullptr;
	Vector<String> submenu_extensions;
};
