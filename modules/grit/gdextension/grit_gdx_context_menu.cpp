#include "grit_gdx_context_menu.h"

#include "grit/grit_gdx_script.h"

#include "grit_gdx_apply.h"
#include "grit_gdx_generator.h"
#include "grit_gdx_job.h"
#include "grit_gdx_manifest.h"

#include "modules/gdscript/gdscript.h"

#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/config/project_settings.h"
#include "core/extension/gdextension_manager.h"
#include "editor/file_system/editor_file_system.h"
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
#include "core/config/project_settings.h"
#include "core/extension/gdextension_manager.h"
#include "editor/editor_interface.h"
#include "editor/editor_node.h"
#include "scene/main/timer.h"

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
	bool rebuildable = false;
	bool legacy = false;
	String revert_class;
	String revert_extension;

	EditorNode *editor = EditorNode::get_singleton();
	if (editor && editor->get_editor_selection()) {
		const List<Node *> selected = editor->get_editor_selection()->get_top_selected_node_list();
		if (selected.size() == 1) {
			Node *node = selected.front()->get();
			makeable = can_make_gdextension(node);
			const Ref<Script> node_script = node->get_script();
			rebuildable = node_script.is_valid() && Object::cast_to<GritGdxScript>(node_script.ptr()) != nullptr;
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
				legacy = GritGdxApply::has_legacy_archive(node_class);
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
	// The archived script is attached to the node, so a rebuild is offered exactly
	// where the archive is visible.
	add_context_menu_item("Rebuild GDExtension", rebuildable ? Callable(callable_mp(this, &GritGdxContextMenuPlugin::_rebuild)) : Callable(), Ref<Texture2D>());
	add_context_menu_item("Migrate Archive to .gds", legacy ? Callable(callable_mp(this, &GritGdxContextMenuPlugin::_migrate)) : Callable(), Ref<Texture2D>());
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
	const String script_path = script->get_path();
	GritGdxApply::Plan plan;
	String plan_error;
	if (GritGdxApply::plan(script_path, plan, plan_error) != OK) {
		_show_message(plan_error);
		return;
	}
	if (plan.converted_functions <= 0) {
		_show_message(vformat("Grit: no method of \"%s\" can be converted yet.\n\n%s", script_path, plan.skipped_text()));
		return;
	}
	if (!plan.is_partial()) {
		_start_add(script_path, submenu_extensions[p_index], scene, path, false);
		return;
	}
	_confirm_partial(script_path, submenu_extensions[p_index], scene, path, plan);
}

void GritGdxContextMenuPlugin::_confirm_partial(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path, const GritGdxApply::Plan &p_plan) {
	if (confirm_dialog) {
		confirm_dialog->queue_free();
		confirm_dialog = nullptr;
	}
	confirm_dialog = memnew(ConfirmationDialog);
	confirm_dialog->set_title("Make GDExtension");
	confirm_dialog->set_ok_button_text("Convert Anyway");
	confirm_dialog->set_min_size(Vector2i(520, 0));

	VBoxContainer *box = memnew(VBoxContainer);
	confirm_dialog->add_child(box);
	Label *label = memnew(Label);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_custom_minimum_size(Vector2i(500, 0));
	label->set_text(vformat("%s.\n\nThese stay behind as stubs that log an error when called:\n%s\nConvert Back restores the script at any time.", p_plan.summary().capitalize(), p_plan.skipped_text()));
	box->add_child(label);

	EditorNode::get_singleton()->get_gui_base()->add_child(confirm_dialog);
	confirm_dialog->popup_centered();
	confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_confirm_partial_add).bind(p_script_path, p_extension, p_scene_path, p_node_path));
}

void GritGdxContextMenuPlugin::_confirm_partial_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path) {
	if (confirm_dialog) {
		confirm_dialog->queue_free();
		confirm_dialog = nullptr;
	}
	_start_add(p_script_path, p_extension, p_scene_path, p_node_path, true);
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
	if (script->get_global_name().is_empty()) {
		_confirm_add_class_name(script->get_path(), scene, path, GritGdxApply::suggested_class_name(script->get_path()));
		return;
	}
	const String default_name = GritGdxGenerator::snake_case(String(script->get_global_name()));
	_prompt_extension_name(script->get_path(), scene, path, default_name);
}

void GritGdxContextMenuPlugin::_prompt_extension_name(const String &p_script_path, const String &p_scene_path, const String &p_node_path, const String &p_default_name) {
	// Pre-flight: what the conversion would produce, before anything is written.
	GritGdxApply::Plan plan;
	String plan_error;
	if (GritGdxApply::plan(p_script_path, plan, plan_error) != OK) {
		_show_message(plan_error);
		return;
	}
	if (plan.converted_functions <= 0) {
		_show_message(vformat("Grit: no method of \"%s\" can be converted yet.\n\n%s", p_script_path, plan.skipped_text()));
		return;
	}

	if (name_dialog) {
		name_dialog->queue_free();
		name_dialog = nullptr;
	}
	name_dialog = memnew(ConfirmationDialog);
	name_dialog->set_title("Make GDExtension");
	name_dialog->set_ok_button_text(plan.is_partial() ? "Convert Anyway" : "Create");
	name_dialog->set_min_size(Vector2i(460, 0));

	VBoxContainer *box = memnew(VBoxContainer);
	name_dialog->add_child(box);
	Label *label = memnew(Label);
	label->set_text("Extension name (one shared library per extension):");
	box->add_child(label);
	name_edit = memnew(LineEdit);
	name_edit->set_text(p_default_name);
	box->add_child(name_edit);

	Label *plan_label = memnew(Label);
	plan_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	plan_label->set_custom_minimum_size(Vector2i(440, 0));
	if (plan.is_partial()) {
		plan_label->set_text(vformat("%s into \"%s\" (%s).\n\nThese stay behind as stubs that log an error when called:\n%s", plan.summary().capitalize(), plan.class_name, plan.base_type, plan.skipped_text()));
	} else {
		plan_label->set_text(vformat("%s into \"%s\" (%s).", plan.summary().capitalize(), plan.class_name, plan.base_type));
	}
	box->add_child(plan_label);

	EditorNode::get_singleton()->get_gui_base()->add_child(name_dialog);
	name_dialog->popup_centered();
	name_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_start_add).bind(p_script_path, String(), p_scene_path, p_node_path, plan.is_partial()));
}

void GritGdxContextMenuPlugin::_start_add(const String &p_script_path, const String &p_extension, const String &p_scene_path, const String &p_node_path, bool p_allow_partial) {
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
	active_scene = p_scene_path;
	job.instantiate();
	const Error error = job->start(p_script_path, extension, p_scene_path, p_node_path, p_allow_partial);
	_start_job();
	if (error != OK) {
		_poll();
	}
}

void GritGdxContextMenuPlugin::_migrate(const Array &p_selection) {
	Vector<String> migrated;
	String report;
	const Error error = GritGdxApply::migrate_legacy(migrated, report);
	if (error == OK && !migrated.is_empty()) {
		// The descriptor gained its class icon and the scenes lost the attached
		// `.gde`, so refresh both.
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->scan_changes();
		}
		GDExtensionManager *manager = GDExtensionManager::get_singleton();
		if (manager) {
			for (const String &directory : GritGdxManifest::list_extensions()) {
				const String descriptor = directory.path_join(GritGdxManifest::extension_name(directory) + ".gdextension");
				if (FileAccess::exists(descriptor) && manager->is_extension_loaded(descriptor)) {
					manager->reload_extension(descriptor);
				}
			}
		}
		if (EditorNode::get_singleton()) {
			EditorData &data = EditorNode::get_editor_data();
			for (int i = 0; i < data.get_edited_scene_count(); i++) {
				const String scene = data.get_scene_path(i);
				if (!scene.is_empty() && FileAccess::exists(scene) && !EditorNode::get_singleton()->is_scene_unsaved(i)) {
					callable_mp(EditorInterface::get_singleton(), &EditorInterface::reload_scene_from_path).bind(scene).call_deferred();
					break;
				}
			}
		}
	}
	_show_message(report.strip_edges());
}

void GritGdxContextMenuPlugin::_rebuild(const Array &p_selection) {
	if (p_selection.size() != 1) {
		return;
	}
	Node *node = Object::cast_to<Node>(p_selection[0]);
	if (!node) {
		return;
	}
	const Ref<Script> script = node->get_script();
	if (script.is_null() || Object::cast_to<GritGdxScript>(script.ptr()) == nullptr) {
		return;
	}
	const String class_name = String(node->get_class_name());
	String scene;
	String path;
	node_location(node, scene, path);
	GritGdxApply::RevertTarget target;
	if (class_name.is_empty() || !GritGdxApply::find_revert_target(class_name, target)) {
		_show_message("Could not find the extension that owns this class.");
		return;
	}
	_close_dialog();
	active_scene = scene;
	job.instantiate();
	const Error error = job->start_rebuild(class_name, target.extension_directory);
	_start_job();
	if (dialog) {
		dialog->set_title("Rebuild GDExtension");
	}
	if (error != OK) {
		_poll();
	}
}

void GritGdxContextMenuPlugin::_start_revert(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path, bool p_replace_script) {
	_close_dialog();
	active_scene = p_scene_path;
	job.instantiate();
	job->start_revert(p_class_name, p_extension, p_scene_path, p_node_path, p_replace_script);
	_start_job();
}

void GritGdxContextMenuPlugin::_confirm_add_class_name(const String &p_script_path, const String &p_scene_path, const String &p_node_path, const String &p_class_name) {
	if (confirm_dialog) {
		confirm_dialog->queue_free();
		confirm_dialog = nullptr;
	}
	confirm_dialog = memnew(ConfirmationDialog);
	confirm_dialog->set_title("Make GDExtension");
	confirm_dialog->set_ok_button_text("Add and Continue");
	confirm_dialog->set_min_size(Vector2i(520, 0));

	VBoxContainer *box = memnew(VBoxContainer);
	confirm_dialog->add_child(box);
	Label *label = memnew(Label);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_custom_minimum_size(Vector2i(500, 0));
	label->set_text(vformat("\"%s\" has no class_name, and conversion turns it into one. Add `class_name %s` to the script and continue?", p_script_path, p_class_name));
	box->add_child(label);

	EditorNode::get_singleton()->get_gui_base()->add_child(confirm_dialog);
	confirm_dialog->popup_centered();
	confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_add_class_name_and_continue).bind(p_script_path, p_scene_path, p_node_path));
}

void GritGdxContextMenuPlugin::_add_class_name_and_continue(const String &p_script_path, const String &p_scene_path, const String &p_node_path) {
	String class_name;
	String add_error;
	if (GritGdxApply::add_class_name(p_script_path, class_name, add_error) != OK) {
		_show_message("Grit: " + add_error);
		return;
	}
	if (EditorFileSystem::get_singleton()) {
		EditorFileSystem::get_singleton()->scan_changes();
	}
	_prompt_extension_name(p_script_path, p_scene_path, p_node_path, GritGdxGenerator::snake_case(class_name));
}

void GritGdxContextMenuPlugin::_confirm_replace(const String &p_class_name, const String &p_extension, const String &p_scene_path, const String &p_node_path, const String &p_script_path, const String &p_archive_path) {
	if (confirm_dialog) {
		confirm_dialog->queue_free();
		confirm_dialog = nullptr;
	}
	confirm_dialog = memnew(ConfirmationDialog);
	confirm_dialog->set_title("Convert Back to GDScript");
	confirm_dialog->set_ok_button_text("Delete and Convert Back");
	confirm_dialog->set_min_size(Vector2i(520, 0));

	VBoxContainer *box = memnew(VBoxContainer);
	confirm_dialog->add_child(box);
	Label *label = memnew(Label);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_custom_minimum_size(Vector2i(500, 0));
	label->set_text(vformat("\"%s\" was added after the conversion. Convert Back will delete it and restore \"%s\" under that name. Rename the script first if you want to keep it.", p_script_path, p_archive_path));
	box->add_child(label);

	EditorNode::get_singleton()->get_gui_base()->add_child(confirm_dialog);
	confirm_dialog->popup_centered();
	confirm_dialog->connect(SceneStringName(confirmed), callable_mp(this, &GritGdxContextMenuPlugin::_start_revert).bind(p_class_name, p_extension, p_scene_path, p_node_path, true));
}

void GritGdxContextMenuPlugin::_start_job() {
	action_finished = false;
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

// The manager writes extension_list.cfg only when an extension is added or
// removed; an extension this action just loaded is already "loaded", so make sure
// the file names it (otherwise the next start cannot find the class).
static void _remember_descriptor(const String &p_descriptor) {
	const String path = ProjectSettings::get_singleton()->get_project_data_path().path_join("extension_list.cfg");
	Vector<String> lines;
	if (FileAccess::exists(path)) {
		Error read_error = OK;
		const String text = FileAccess::get_file_as_string(path, &read_error);
		if (read_error == OK) {
			for (const String &line : text.split("\n")) {
				const String entry = line.strip_edges();
				if (!entry.is_empty()) {
					lines.push_back(entry);
				}
			}
		}
	}
	if (lines.has(p_descriptor)) {
		return;
	}
	lines.push_back(p_descriptor);
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
	if (file.is_valid()) {
		for (const String &entry : lines) {
			file->store_line(entry);
		}
	}
}

// Defined after _poll; queued from it to run after the scene reload.
static void _sync_extensions_after_action(const String &p_descriptor);

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

	if (error == OK) {
		// A new library is not registered until the manager loads it; adding a class
		// rebuilds a library the editor already has open, so reload it then. The
		// scene reload is deferred, and the unload of a removed extension (revert of
		// the last class) is queued after it: the old scene's instances must be gone
		// before their classes disappear, or tearing them down crashes.
		const String directory = job->get_extension_directory();
		const String descriptor = directory.is_empty() ? String() : directory.path_join(GritGdxManifest::extension_name(directory) + ".gdextension");
		GDExtensionManager *manager = GDExtensionManager::get_singleton();
		const bool descriptor_exists = !descriptor.is_empty() && FileAccess::exists(descriptor);
		if (manager && descriptor_exists) {
			const bool loaded = manager->is_extension_loaded(descriptor);
			const GDExtensionManager::LoadStatus status = loaded ? manager->reload_extension(descriptor) : manager->load_extension(descriptor);
			print_line(status == GDExtensionManager::LOAD_STATUS_OK ? vformat("Grit: %s \"%s\".", loaded ? "reloaded" : "loaded", descriptor) : vformat("Grit: \"%s\" is not active; restart the editor to use its classes.", descriptor));
			if (status == GDExtensionManager::LOAD_STATUS_OK) {
				_remember_descriptor(descriptor);
			}
		}
		// The converted class replaces the script's global class name; rescan before
		// the scene reload so the GDScript class cache drops the old `.gd`.
		if (EditorFileSystem::get_singleton()) {
			EditorFileSystem::get_singleton()->scan_changes();
		}
		if (!active_scene.is_empty() && EditorNode::get_singleton()) {
			EditorData &data = EditorNode::get_editor_data();
			for (int i = 0; i < data.get_edited_scene_count(); i++) {
				if (data.get_scene_path(i) != active_scene) {
					continue;
				}
				if (EditorNode::get_singleton()->is_scene_unsaved(i)) {
					print_line(vformat("Grit: \"%s\" has unsaved changes; reopen it to see the result.", active_scene));
				} else {
					callable_mp(EditorInterface::get_singleton(), &EditorInterface::reload_scene_from_path).bind(active_scene).call_deferred();
				}
				break;
			}
		}
		callable_mp_static(&_sync_extensions_after_action).bind(descriptor).call_deferred();
	}
	active_scene = String();

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
	action_finished = true;
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
	// Main thread: the revert itself runs on a worker and must not touch the
	// script editor's control tree.
	GritGdxApply::close_editor_script_tabs(target.archived_script);
	GritGdxApply::close_editor_script_tabs(target.script_path);
	// A script added after the conversion is only replaced with confirmation (an
	// identical copy needs none; revert() handles that case itself).
	bool identical = false;
	if (GritGdxApply::has_replacement_script(class_name, identical) && !identical) {
		_confirm_replace(class_name, extension, scene, path, target.script_path, target.archived_script);
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

static void _sync_extensions_after_action(const String &p_descriptor) {
	GDExtensionManager *manager = GDExtensionManager::get_singleton();
	if (!manager) {
		return;
	}
	if (!p_descriptor.is_empty() && !FileAccess::exists(p_descriptor) && manager->is_extension_loaded(p_descriptor)) {
		manager->unload_extension(p_descriptor);
		print_line(vformat("Grit: unloaded \"%s\" (extension removed).", p_descriptor));
	}
	// Keep .godot/extension_list.cfg in sync: a deleted extension must not be loaded
	// on the next start, and a new one must be.
	HashSet<String> descriptors;
	for (const String &loaded : manager->get_loaded_extensions()) {
		if (FileAccess::exists(loaded)) {
			descriptors.insert(loaded);
		}
	}
	Ref<DirAccess> root = DirAccess::open("res://gdext");
	if (root.is_valid()) {
		for (const String &subdirectory : root->get_directories()) {
			Ref<DirAccess> folder = DirAccess::open("res://gdext/" + subdirectory);
			if (folder.is_null()) {
				continue;
			}
			for (const String &file : folder->get_files()) {
				if (file.get_extension() == "gdextension") {
					descriptors.insert("res://gdext/" + subdirectory + "/" + file);
				}
			}
		}
	}
	manager->ensure_extensions_loaded(descriptors);
}

// ---------------------------------------------------------------------------
// GUI autotest harness
// ---------------------------------------------------------------------------

static String _autotest_state(Node *p_node) {
	if (!p_node) {
		return "no node";
	}
	const Ref<Script> script = p_node->get_script();
	const Dictionary badge = EditorNode::get_singleton()->get_class_badge(p_node->get_class_name());
	return vformat("class=%s script=%s badge=%s", p_node->get_class_name(), script.is_valid() ? script->get_path() : String("<none>"), badge.get("icon", StringName("<none>")));
}

void GritGdxContextMenuPlugin::run_autotest(const String &p_scene_path) {
	autotest_scene = p_scene_path;
	autotest_step = 0;
	autotest_timer = memnew(Timer);
	autotest_timer->set_wait_time(1.0);
	autotest_timer->set_one_shot(false);
	EditorNode::get_singleton()->get_gui_base()->add_child(autotest_timer);
	autotest_timer->connect(SNAME("timeout"), callable_mp(this, &GritGdxContextMenuPlugin::_autotest_tick));
	autotest_timer->start();
}

void GritGdxContextMenuPlugin::_autotest_tick() {
	EditorNode *editor = EditorNode::get_singleton();
	Node *root = editor ? editor->get_edited_scene() : nullptr;
	if (!root) {
		// Keep asking: the editor ignores open requests while its first scan runs.
		print_line("Grit autotest: opening " + autotest_scene);
		EditorInterface::get_singleton()->open_scene_from_path(autotest_scene);
		return;
	}
	EditorSelection *selection = editor->get_editor_selection();
	Node *child = root->get_node_or_null(NodePath("Child"));

	if (!action_finished) {
		return; // _poll() has not completed (and reloaded the scene) yet
	}

	switch (autotest_step) {
		case 0: {
			print_line(vformat("Grit autotest [0] opened: root %s | child %s", _autotest_state(root), _autotest_state(child)));
			if (!child) {
				print_line("Grit autotest: no Child node, stopping");
				autotest_timer->stop();
				return;
			}
			selection->clear();
			selection->add_node(child);
			const Ref<Script> child_script = child->get_script();
			if (child_script.is_null()) {
				print_line("Grit autotest: the child has no script, stopping");
				autotest_timer->stop();
				return;
			}
			print_line(vformat("Grit autotest [1] converting the child from %s", child_script->get_path()));
			_start_add(child_script->get_path(), "res://gdext/auto", autotest_scene, "Child", false);
			autotest_step = 1;
		} break;
		case 1: {
			print_line("Grit autotest [2] child converted; in-memory tree says: " + _autotest_state(root->get_node_or_null(NodePath("Child"))));
			Node *live_child = root->get_node_or_null(NodePath("Child"));
			if (live_child) {
				selection->clear();
				selection->add_node(live_child);
			}
			const Ref<Script> root_script = root->get_script();
			if (root_script.is_null()) {
				print_line("Grit autotest: the root has no script, stopping");
				autotest_timer->stop();
				return;
			}
			print_line(vformat("Grit autotest [3] converting the root from %s into the same extension", root_script->get_path()));
			_start_add(root_script->get_path(), "res://gdext/auto", autotest_scene, ".", false);
			autotest_step = 2;
		} break;
		case 2: {
			print_line("Grit autotest [4] both converted; in-memory tree says: root " + _autotest_state(root) + " | child " + _autotest_state(root->get_node_or_null(NodePath("Child"))));
			print_line(vformat("Grit autotest [5] file says: root type=%s", GritGdxApply::scene_node_type(autotest_scene, ".")));
			const String root_type = GritGdxApply::scene_node_type(autotest_scene, ".");
			print_line(vformat("Grit autotest [6] scene reload; \"%s\" registered=%s", root_type, ClassDB::class_exists(root_type) ? "yes" : "no"));
			print_line(vformat("Grit autotest [6b] extension_list: %s", FileAccess::get_file_as_string(ProjectSettings::get_singleton()->get_project_data_path().path_join("extension_list.cfg")).strip_edges().replace("\n", " | ")));
			if (!OS::get_singleton()->get_environment("GRIT_GDX_AUTOTEST_CONVERT_ONLY").is_empty()) {
				print_line("Grit autotest: stopping after the conversions");
				autotest_timer->stop();
				EditorNode::get_singleton()->get_tree()->quit();
				return;
			}
			EditorInterface::get_singleton()->reload_scene_from_path(autotest_scene);
			autotest_step = 3;
		} break;
		case 3: {
			Node *reloaded = editor->get_edited_scene();
			print_line(vformat("Grit autotest [7] after reload: root %s | child %s", _autotest_state(reloaded), _autotest_state(reloaded ? reloaded->get_node_or_null(NodePath("Child")) : nullptr)));
			if (!reloaded) {
				print_line("Grit autotest: scene did not reload, stopping");
				autotest_timer->stop();
				return;
			}
			Node *live_child = reloaded->get_node_or_null(NodePath("Child"));
			// Touch the archives so the class badge goes stale, and print what the
			// scene tree would draw.
			const auto make_stale = [](Node *p_node) {
				GritGdxApply::RevertTarget target;
				if (!p_node || !GritGdxApply::find_revert_target(p_node->get_class_name(), target) || !FileAccess::exists(target.archived_script)) {
					return;
				}
				Error error = OK;
				const String text = FileAccess::get_file_as_string(target.archived_script, &error);
				if (error != OK) {
					return;
				}
				Ref<FileAccess> file = FileAccess::open(target.archived_script, FileAccess::WRITE);
				if (file.is_valid()) {
					file->store_string(text + "# made stale by the autotest\n");
				}
			};
			make_stale(reloaded);
			make_stale(live_child);
			const Dictionary root_badge = EditorNode::get_singleton()->get_class_badge(reloaded->get_class_name());
			const Dictionary child_badge = live_child ? EditorNode::get_singleton()->get_class_badge(live_child->get_class_name()) : Dictionary();
			print_line(vformat("Grit autotest [7b] stale badge: root=%s child=%s", root_badge.get("icon", StringName("<none>")), child_badge.get("icon", StringName("<none>"))));
			selection->clear();
			selection->add_node(live_child ? live_child : reloaded);
			print_line("Grit autotest [8] converting the child back");
			Array nodes;
			nodes.push_back(live_child ? live_child : reloaded);
			_convert_back(nodes);
			autotest_step = 4;
		} break;
		case 4: {
			Node *reloaded = editor->get_edited_scene();
			print_line(vformat("Grit autotest [9] child reverted; in-memory: %s", _autotest_state(reloaded->get_node_or_null(NodePath("Child")))));
			selection->clear();
			selection->add_node(reloaded);
			print_line("Grit autotest [10] converting the root back");
			Array nodes;
			nodes.push_back(reloaded);
			_convert_back(nodes);
			autotest_step = 5;
		} break;
		case 5: {
			Node *reloaded = editor->get_edited_scene();
			print_line(vformat("Grit autotest [11] root reverted; in-memory: %s", _autotest_state(reloaded)));
			print_line(vformat("Grit autotest [12] file says: root type=%s", GritGdxApply::scene_node_type(autotest_scene, ".")));
			print_line("Grit autotest: done");
			autotest_timer->stop();
			EditorNode::get_singleton()->get_tree()->quit();
			autotest_step = 6;
		} break;
		default:
			break;
	}
}
