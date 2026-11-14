#pragma once

#include "editor/export/editor_export_plugin.h"

class GritExportPlugin : public EditorExportPlugin {
	GDCLASS(GritExportPlugin, EditorExportPlugin);

	HashMap<String, String> stripped_sources;

	enum ExportMode {
		EXPORT_VM_DEBUG,
		EXPORT_VM_RELEASE,
		EXPORT_COMPILED_DEBUG,
		EXPORT_COMPILED_RELEASE,
	};

	ExportMode export_mode = EXPORT_COMPILED_RELEASE;
	mutable Ref<EditorExportPlatform> last_platform;
	String export_platform;
	// The preset's own custom templates, restored when the export ends: a
	// compiled export must never leave its template behind in export_presets.cfg.
	String previous_template_debug;
	String previous_template_release;
	bool templates_saved = false;
	// Set by prepare_export(): code generation and the template build already ran.
	bool prepared_compiled = false;
	bool prepared_debug = false;
	PackedByteArray prepared_fingerprint;

	Error _prepare_compiled(const Ref<EditorExportPreset> &p_preset, bool p_debug);
	Error _generate_and_build(bool p_debug, String &r_template_path);
	String _compiled_template_path(bool p_debug) const;
	bool _template_needs_rebuild(const String &p_template_path) const;
	// Finds the engine tree on first use (and saves it as a project setting), then
	// makes sure a VM export has a stock template to fall back on.
	String _resolve_engine_root() const;
	mutable String resolved_engine_root;
	// One install attempt per editor session: the export dialog re-validates
	// often, and a failed build must not be retried on every refresh.
	mutable bool template_install_tried = false;

	Error _build_template(const String &p_generated_dir, bool p_debug, String &r_template_path);
	uint64_t customization_hash = 0;
	EditorExportPreset::ScriptExportMode script_mode = EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED;

protected:
	virtual Error prepare_export(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path) override;
	virtual void _export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) override;
	virtual void _export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) override;
	virtual void _export_end() override;
	virtual bool _begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) override;
	virtual Ref<Resource> _customize_resource(const Ref<Resource> &p_resource, const String &p_path) override;
	virtual uint64_t _get_customization_configuration_hash() const override;
	virtual void _get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const override;

public:
	static constexpr const char *SETTING_GENERATED_DIRECTORY = "grit/export/generated_directory";
	static constexpr const char *OPTION_STRIP_SCRIPTS = "grit/strip_scripts";

	// Runs when the user adds an export preset: checks the build environment and
	// builds/installs the stock template if it is missing.
	virtual void export_platform_added(const Ref<EditorExportPreset> &p_preset) override;
	virtual void export_platform_removed(const Ref<EditorExportPreset> &p_preset) override;
	virtual void export_dialog_opened() override;

	// Makes sure the project has this platform's Fast preset. Called when the export
	// window opens, so the preset is there without anything being downloaded.
	void _ensure_native_preset();

	virtual String get_name() const override { return "Grit"; }
};
