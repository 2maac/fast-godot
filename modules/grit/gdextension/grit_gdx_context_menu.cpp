#include "grit_gdx_context_menu.h"

#include "grit_gdx_apply.h"
#include "grit_gdx_generator.h"
#include "grit_gdx_job.h"
#include "grit_gdx_manifest.h"

#include "modules/gdscript/gdscript.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/extension/gdextension_manager.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/gui/box_container.h"
#include "scene/gui/dialogs.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/popup_menu.h"
#include "scene/gui/progress_bar.h"
#include "servers/text/text_server.h"
#include "scene/main/node.h"
#include "scene/main/scene_tree.h"
#include "scene/main/timer.h"
#include "scene/scene_string_names.h"
#include "scene/resources/packed_scene.h"

#include "editor/editor_data.h"
#include "editor/editor_node.h"

static bool is_generated_class(const StringName &p_class) {
	if (!ClassDB::class_exists(p_class)) {
		return false;
	}
	const ClassDB::APIType api = ClassDB::get_api_type(p_class);
	return api == ClassDB::API_EXTENSION || api == ClassDB::API_EDITOR_EXTENSION;
}

// Folder owning a generated class, or empty. Handles both layouts.
static String extension_of_class(const String &p_class) {
	GritGdxApply::RevertTarget target;
	return GritGdxApply::find_revert_target(p_class, target) ? target.extension_directory : String();
}

// Scene the node is saved in plus its path relative to the edited scene root.
static bool node_location(Node *p_node, String &r_scene, String &r_path) {
	Node *root = EditorNode::get_singleton() ? EditorNode::get_singleton()->get_edited_scene() : nullptr;
	if (!root || !p_node) {
		return false;
	}
	// The scene root has no owner; anything else must belong to the edited scene.
	if (p_node != root && p_node->get_owner() != root) {
		return false;
	}
	r_scene = root->get_scene_file_path();
	if (r_scene.is_empty()) {
		return false;
	}
	r_path = String(root->get_path_to(p_node));
	return !r_path.is_empty();
}

void GritGdxContextMenuPlugin::_show_message(const String &p_text) {
	AcceptDialog *dialog = memnew(AcceptDialog);
	dialog->set_title("Make GDExtension");
	dialog->set_min_size(Vector2i(480, 0));
	dialog->set_ok_button_text("Close");
	dialog->set_text(p_text);
	EditorNode::get_singleton()->get_gui_base()->add_child(dialog);
	dialog->popup_centered();
	dialog->connect(SceneStringName(confirmed), Callable(dialog, SNAME("queue_free")));
}

bool GritGdxContextMenuPlugin::can_make_gdextension(Node *p_node) {
	if (!p_node || is_generated_class(p_node->get_class_name())) {
		return false;
	}
	const Ref<GDScript> script = p_node->get_script();
	if (script.is_null()) {
		return false;
	}
	return !is_generated_class(script->get_instance_base_type());
}

GritGdxContextMenuPlugin::~GritGdxContextMenuPlugin() {
	if (submenu) {
		memdelete(submenu);
	}
}

void GritGdxContextMenuPlugin::get_options(const Vector<String> &p_paths) {
	bool single = false;
	bool makeable = false;
	bool revertable = false;
	String revert_class;
	String revert_extension;

	EditorNode *editor = EditorNode::get_singleton();
	if (editor && editor->get_editor_selection()) {
		const List<Node *> selected = editor->get_editor_selection()->get_top_selected_node_list();
		if (selected.size() == 1) {
			Node *node = selected.front()->get();
			makeable = can_make_gdextension(node);
			// The editor may still hold the scene it loaded before a conversion, so
			// the node's in-memory type can be stale; trust the scene file.
			String node_class = String(node->get_class_name());
			String scene;
			String path;
			if (!is_generated_class(node_class) && node_location(node, scene, path)) {
				const String file_type = GritGdxApply::scene_node_type(scene, path);
				if (!file_type.is_empty()) {
					node_class = file_type;
				}
			}
			GritGdxApply::RevertTarget target;
			if (!node_class.is_empty() && GritGdxApply::find_revert_target(node_class, target)) {
				revert_class = node_class;
				revert_extension = target.extension_directory;
				// Offered for any generated node; the class itself is only dropped
				// when no other node uses it.
				revertable = true;
			}
			single = true;
		}
	}
	// Enabled states deliberately do not depend on the node being locatable: a
	// root node or an unsaved scene still runs the action and explains itself
	// through a dialog instead of silently greying the item out.

	add_context_menu_item("Make GDExtension...", makeable ? Callable(callable_mp(this, &GritGdxContextMenuPlugin::_make_gdextension)) : Callable(), Ref<Texture2D>());

	submenu_extensions = GritGdxManifest::list_extensions();
	if (makeable && !submenu_extensions.is_empty()) {
		if (!submenu) {
			submenu = memnew(PopupMenu);
			submenu->connect(SceneStringName(id_pressed), callable_mp(this, &GritGdxContextMenuPlugin::_submenu_selected));
		}
		submenu->clear();
		for (uint32_t i = 0; i < submenu_extensions.size(); i++) {
			submenu->add_item(String(submenu_extensions[i]).get_file(), int(i));
		}
		add_context_submenu_item("Add to Existing Extension", submenu, Ref<Texture2D>());
	} else {
		// Greyed out: no usable node, or no extension yet.
		add_context_menu_item("Add to Existing Extension", Callable(), Ref<Texture2D>());
	}

	add_context_menu_item("Convert Back to GDScript", revertable ? Callable(callable_mp(this, &GritGdxContextMenuPlugin::_convert_back)) : Callable(), Ref<Texture2D>());
}

void GritGdxContextMenuPlugin::_submenu_selected(int p_index) {
	if (p_index < 0 || p_index >= int(submenu_extensions.size())) {
		return;
	}
	EditorNode *editor = EditorNode::get_singleton();
	if (!editor || !editor->get_editor_selection()) {
		return;
	}
	Array selection;
	for (Node *node : editor->get_editor_selection()->get_top_selected_node_list()) {
		selection.push_back(node);
	}
	if (selection.size() != 1) {
		return;
	}
	Node *node = Object::cast_to<Node>(selection[0]);
	if (!node) {
		return;
	}
	String scene;
	String path;
	if (!node_location(node, scene, path)) {
		_show_message("Save the scene first: the node must belong to the edited scene.");
		return;
	}
	if (!can_make_gdextension(node)) {
		return;
	}
	const Ref<GDScript> script = node->get_script();
	_start_add(script->get_path(), submenu_extensions[p_index], scene, path);
}

void GritGdxContextMenuPlugin::_make_gdextension(const Array &p_selection) {
	if (p_selection.size() != 1) {
		return;
	}
	Node *node = Object::cast_to<Node>(p_selection[0]);
	String scene;
	String path;
	if (node && !node_location(node, scene, path)) {
		_show_message(vformat("\"%s\" is not part of the edited scene.\n\nSave the scene and select a node in it, then try again.", node->get_name()));
		return;
	}
	if (!node || !can_make_gdextension(node)) {
		return;
	}
	const Ref<GDScript> script = node->get_script();
	const String default_name = GritGdxGenerator::snake_case(String(script->get_global_name()).is_empty() ? script->get_path().get_file().get_basename() : String(script->get_global_name()));
	_prompt_extension_name(script->get_path(), scene, path, default_name);
}

void GritGdxContextMenuPlugin::_prompt_extension_name(const String &p_script_path, const String &p_scene_path, const String &p_node_path, const String &p_default_name) {
	if (name_dialog) {
		name_dialog->queue_free();
		name_dialog = nullptr;
	}
	name_dialog = memnew(ConfirmationDialog);
	name_dialog->set_title("Make GDExtension");
	name_dialog->set_ok_button_text("Create");
	name_dialog->set_min_size(Vector2i(420, 0));

	VBoxContainer *box = memnew(VBoxContainer);
	name_dialog->add_child(box);
	Label *label = memnew(Label);
	label->set_text("Extension name (one shared library per extension):");
	box->add_child(label);
	name_edit = memnew(LineEdit);
	name_edit->set_text(p_default_name);
	box->add_child(name_edit);

	EditorNode::get_singleton()->get_gui_base()->add_child(name_dialog);
	name_dialog->popup_centered();
	name_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_start_add).bind(p_script_path, String(), p_scene_path, p_node_path));
}

void GritGdxContextMenuPlugin::_start_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path) {
	String extension = p_extension;
	if (extension.is_empty()) {
		if (!name_edit) {
			return;
		}
		extension = "res://gdext/" + GritGdxGenerator::snake_case(name_edit->get_text().strip_edges());
	}
	if (name_dialog) {
		name_dialog->queue_free();
		name_dialog = nullptr;
		name_edit = nullptr;
	}

	_close_dialog();
	job.instantiate();
	const Error error = job->start(p_script_path, extension, p_scene_path, p_node_path);
	_start_job();
	if (error != OK) {
		_poll();
	}
}

void GritGdxContextMenuPlugin::_start_revert(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path) {
	_close_dialog();
	job.instantiate();
	job->start_revert(p_class_name, p_extension, p_scene_path, p_node_path);
	_start_job();
}

void GritGdxContextMenuPlugin::_start_job() {
	dialog = memnew(AcceptDialog);
	dialog->set_title("Make GDExtension");
	dialog->set_min_size(Vector2i(560, 0));
	dialog->get_ok_button()->set_disabled(true);
	dialog->set_ok_button_text("Close");

	VBoxContainer *box = memnew(VBoxContainer);
	dialog->add_child(box);
	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->set_custom_minimum_size(Vector2i(540, 0));
	box->add_child(status_label);
	progress_bar = memnew(ProgressBar);
	progress_bar->set_show_percentage(false);
	progress_bar->set_custom_minimum_size(Vector2i(540, 18));
	progress_bar->set_indeterminate(true);
	box->add_child(progress_bar);
	status_label->set_text(job.is_valid() ? job->get_status() : "");

	EditorNode::get_singleton()->get_gui_base()->add_child(dialog);
	dialog->popup_centered();
	dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_close_dialog));

	timer = memnew(Timer);
	timer->set_wait_time(0.25);
	dialog->add_child(timer);
	timer->connect("timeout", callable_mp(this, &GritGdxContextMenuPlugin::_poll));
	timer->start();
}

void GritGdxContextMenuPlugin::_poll() {
	if (job.is_null()) {
		return;
	}
	if (!job->is_finished()) {
		if (status_label) {
			status_label->set_text(job->get_status());
		}
		if (progress_bar) {
			progress_bar->set_value(job->get_progress());
		}
		return;
	}

	if (timer) {
		timer->stop();
	}
	String report;
	const Error error = job->complete(report);
	print_line(report);

	if (status_label) {
		status_label->set_text(error == OK ? report : report + "\nGrit: the action failed; see the report above.");
	}
	if (progress_bar) {
		progress_bar->set_indeterminate(false);
		progress_bar->set_value(error == OK ? 100 : 0);
	}
	if (dialog) {
		dialog->get_ok_button()->set_disabled(false);
	}
}

void GritGdxContextMenuPlugin::_close_dialog() {
	if (timer) {
		timer->queue_free();
		timer = nullptr;
	}
	if (dialog) {
		dialog->queue_free();
		dialog = nullptr;
	}
	status_label = nullptr;
	progress_bar = nullptr;
	job.unref();
}

void GritGdxContextMenuPlugin::_convert_back(const Array &p_selection) {
	if (p_selection.size() != 1) {
		return;
	}
	Node *node = Object::cast_to<Node>(p_selection[0]);
	if (!node) {
		return;
	}
	String scene;
	String path;
	if (!node_location(node, scene, path)) {
		_show_message("Save the scene first: the node must belong to the edited scene.");
		return;
	}
	String class_name = String(node->get_class_name());
	if (!is_generated_class(class_name)) {
		// Stale in-memory scene: use the type written in the file.
		class_name = GritGdxApply::scene_node_type(scene, path);
	}
	GritGdxApply::RevertTarget target;
	if (class_name.is_empty() || !GritGdxApply::find_revert_target(class_name, target)) {
		return;
	}
	const String extension = target.extension_directory;
	if (extension.is_empty()) {
		print_line("Grit: could not find the extension that owns \"" + class_name + "\".");
		return;
	}
	_start_revert(class_name, extension, scene, path);
}

void GritGdxContextMenuPlugin::run_menu_check(const String &p_scene_path) {
	Error error = OK;
	const Ref<PackedScene> scene = ResourceLoader::load(p_scene_path, "", ResourceFormatLoader::CACHE_MODE_IGNORE, &error);
	Node *root = (error == OK && scene.is_valid()) ? scene->instantiate() : nullptr;
	if (!root) {
		print_line(vformat("Grit: could not load \"%s\".", p_scene_path));
	}
	if (root) {
		Vector<Node *> nodes = { root };
		for (int i = 0; i < root->get_child_count(); i++) {
			nodes.push_back(root->get_child(i));
		}
		for (Node *node : nodes) {
			const Ref<Script> script = node->get_script();
			print_line(vformat("Grit: node \"%s\" type \"%s\" script \"%s\" makeable=%s", node->get_name(), node->get_class_name(), script.is_valid() ? script->get_path() : String("<none>"), can_make_gdextension(node) ? "yes" : "no"));
		}
		memdelete(root);
	}
	print_line(vformat("Grit: extensions: %d", GritGdxManifest::list_extensions().size()));

	PopupMenu *popup = memnew(PopupMenu);
	EditorContextMenuPluginManager::get_singleton()->add_options_from_plugins(popup, EditorContextMenuPlugin::CONTEXT_SLOT_SCENE_TREE, Vector<String>());
	for (int i = 0; i < popup->get_item_count(); i++) {
		print_line(vformat("Grit: menu item \"%s\" disabled=%s (no editor selection in this check)", popup->get_item_text(i), popup->is_item_disabled(i) ? "yes" : "no"));
	}
	memdelete(popup);

	SceneTree *tree = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	if (tree) {
		tree->quit();
	}
}
