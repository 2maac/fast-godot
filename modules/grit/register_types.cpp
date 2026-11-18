#include "register_types.h"

#include "grit_gdx_script.h"
#include "runtime/grit_registry.h"
#include "runtime/grit_runtime.h"
#include "runtime/grit_runtime_code_generator.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"
#include "core/object/class_db.h"
#include "modules/gdscript/gdscript_compiler.h"

#ifdef TOOLS_ENABLED
#include "compiler/grit_recording_code_generator.h"
#include "editor/grit_export_plugin.h"
#include "editor/grit_settings_editor.h"
#include "editor/grit_provisioner.h"
#include "gdextension/grit_gdx_apply.h"
#include "gdextension/grit_gdx_context_menu.h"
#include "gdextension/grit_gdx_generator.h"

#include "core/config/project_settings.h"
#include "core/version.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/script/script_create_dialog.h"
#include "editor/inspector/editor_context_menu_plugin.h"
#include "editor/export/editor_export.h"
#endif

static GritRegistry *grit_registry = nullptr;

static GDScriptCodeGenerator *grit_create_code_generator() {
#ifdef TOOLS_ENABLED
	if (GritScriptRecording::get_current()) {
		return memnew(GritRecordingCodeGenerator);
	}
#endif
	if (grit_registry->is_active()) {
		return memnew(GritRuntimeCodeGenerator);
	}
	return memnew(GDScriptByteCodeGenerator);
}

#ifdef TOOLS_ENABLED
static void grit_editor_init() {
	// EditorSettings exists by now; registering it earlier would be a no-op.
	GritProvisioner::register_settings();
	GritProvisioner::purge_obsolete_settings();
	// Draws the source row (Fetch Source button + path entry) in Editor Settings.
	EditorInspector::add_inspector_plugin(Ref<EditorInspectorPlugin>(memnew(GritSettingsPlugin)));
	// Class badge: a stale generated class gets an overlay on its scene-tree icon.
	EditorNode::get_singleton()->add_class_badge_provider(callable_mp_static(&GritGdxApply::class_stale_badge));
	Ref<GritExportPlugin> export_plugin;
	export_plugin.instantiate();
	EditorExport *editor_export = EditorExport::get_singleton();
	const Vector<Ref<EditorExportPlugin>> other_plugins = editor_export->get_export_plugins();
	for (const Ref<EditorExportPlugin> &plugin : other_plugins) {
		editor_export->remove_export_plugin(plugin);
	}
	editor_export->add_export_plugin(export_plugin);
	// Debug aid: GRIT_PROBE_NOW=1 runs the provisioning probe at startup so it can be
	// exercised headlessly (the normal trigger is opening the export window).
	if (!OS::get_singleton()->get_environment("GRIT_PROBE_NOW").is_empty()) {
		callable_mp_static(&GritProvisioner::run_probe).call_deferred();
	}
	print_line(vformat("Grit: GDExtension tools active (build %s).", String(VERSION_HASH).is_empty() ? String(VERSION_FULL_BUILD) : vformat("%s.%s", VERSION_FULL_BUILD, VERSION_HASH)));

	// GDExtension sources are created from FileSystem > New Script with the
	// "GDScript (GDExtension)" language; the scene tree has no Grit actions.
	ScriptCreateDialog::register_extra_language("GDScript (GDExtension)", "gde", "class_name %s", &GritGdxApply::create_source_callback);
	const List<String> arguments = OS::get_singleton()->get_cmdline_args();
	for (const List<String>::Element *element = arguments.front(); element; element = element->next()) {
		const String &argument = element->get();
		if (argument == "--grit-gdx-dialog-check") {
			callable_mp_static(&ScriptCreateDialog::run_language_check).call_deferred();
			break;
		}
		if (argument == "--grit-gdx" || argument == "--grit-gdx-add" || argument == "--grit-gdx-generate" || argument == "--grit-gdx-apply" || argument == "--grit-gdx-revert" || argument == "--grit-gdx-rebuild" || argument == "--grit-gdx-migrate" || argument == "--grit-gdx-plan") {
			callable_mp_static(&GritGdxGenerator::run_from_command_line).call_deferred();
			break;
		}
	}
	for (const Ref<EditorExportPlugin> &plugin : other_plugins) {
		editor_export->add_export_plugin(plugin);
	}
}
#endif

static Ref<GritGdxScriptLoader> grit_gdx_script_loader;
static Ref<GritGdxScriptSaver> grit_gdx_script_saver;

void initialize_grit_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		// Converted nodes keep their archived source attached as a `.gde` script.
		// It never instantiates, so it must exist in exported games too: the scene
		// references it and the loader is what makes that reference resolvable.
		GDREGISTER_CLASS(GritGdxScript);
		grit_gdx_script_loader.instantiate();
		ResourceLoader::add_resource_format_loader(grit_gdx_script_loader);
		return;
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		grit_gdx_script_saver.instantiate();
		ResourceSaver::add_resource_format_saver(grit_gdx_script_saver);
		return;
	}
#endif
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	grit_registry = memnew(GritRegistry);
	grit_registry->activate();

#ifdef TOOLS_ENABLED
	// Internal state only: nothing Grit owns is meant to be edited in Project
	// Settings, so every key here is registered with NO_EDITOR and Grit does not
	// appear as a project settings section at all.
	const uint32_t internal = PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NO_EDITOR;
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "grit/export/build_profile", PROPERTY_HINT_NONE, "", internal), "");
	GLOBAL_DEF(PropertyInfo(Variant::STRING, GritExportPlugin::SETTING_GENERATED_DIRECTORY, PROPERTY_HINT_NONE, "", internal), "res://.godot/grit/generated");
	EditorNode::add_init_callback(grit_editor_init);
	GDScriptCompiler::set_code_generator_factory(grit_create_code_generator);
#else
	if (grit_registry->is_active()) {
		GDScriptCompiler::set_code_generator_factory(grit_create_code_generator);
	}
#endif
}

void uninitialize_grit_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		if (grit_gdx_script_loader.is_valid()) {
			ResourceLoader::remove_resource_format_loader(grit_gdx_script_loader);
			grit_gdx_script_loader.unref();
		}
		return;
	}
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		if (grit_gdx_script_saver.is_valid()) {
			ResourceSaver::remove_resource_format_saver(grit_gdx_script_saver);
			grit_gdx_script_saver.unref();
		}
		return;
	}
#endif
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GDScriptCompiler::set_code_generator_factory(nullptr);
	GritRuntime::clear_property_accessors();
	memdelete(grit_registry);
	grit_registry = nullptr;
}
