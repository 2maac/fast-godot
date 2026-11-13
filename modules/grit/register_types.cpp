#include "register_types.h"

#include "runtime/grit_registry.h"
#include "runtime/grit_runtime.h"
#include "runtime/grit_runtime_code_generator.h"

#include "modules/gdscript/gdscript_compiler.h"

#ifdef TOOLS_ENABLED
#include "compiler/grit_recording_code_generator.h"
#include "editor/grit_export_plugin.h"
#include "editor/grit_settings_editor.h"
#include "editor/grit_provisioner.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
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
	for (const Ref<EditorExportPlugin> &plugin : other_plugins) {
		editor_export->add_export_plugin(plugin);
	}
}
#endif

void initialize_grit_module(ModuleInitializationLevel p_level) {
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
	if (p_level != MODULE_INITIALIZATION_LEVEL_SERVERS) {
		return;
	}

	GDScriptCompiler::set_code_generator_factory(nullptr);
	GritRuntime::clear_property_accessors();
	memdelete(grit_registry);
	grit_registry = nullptr;
}
