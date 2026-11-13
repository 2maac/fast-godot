#include "grit_export_plugin.h"

#include "grit_project_compiler.h"

#include "grit/runtime/grit_registry.h"

#include "modules/gdscript/gdscript.h"
#include "modules/gdscript/gdscript_tokenizer_buffer.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/resource.h"
#include "core/input/input.h"

#ifndef _WIN32
#include <signal.h>
#endif
#include "core/os/os.h"
#include "servers/display/display_server.h"

#include "core/version.h"
#include "editor/editor_node.h"
#include "modules/grit/editor/grit_provisioner.h"
#include "editor/export/editor_export.h"
#include "platform/macos/export/export_plugin_fast.h"
#include "editor/file_system/editor_paths.h"
#include "editor/settings/editor_settings.h"

static String _safe_project_name() {
	String project_name = GLOBAL_GET("application/config/name");
	if (project_name.is_empty()) {
		project_name = "project";
	}
	String safe;
	for (int i = 0; i < project_name.length(); i++) {
		const char32_t c = project_name[i];
		safe += (is_ascii_alphanumeric_char(c) || c == '-' || c == '_') ? String::chr(c) : String("_");
	}
	return safe;
}

// macOS refuses to export an app without a bundle identifier ("Invalid bundle
// identifier: Identifier is missing"), and a fresh preset ships the option empty,
// which makes the user stop and invent one. Fill in "fast.<project>" instead:
// identifiers allow letters, digits, hyphens and dots, so everything else becomes
// a hyphen and the whole thing is lowercased.
static String _default_bundle_identifier() {
	String project_name = GLOBAL_GET("application/config/name");
	if (project_name.is_empty()) {
		project_name = "project";
	}
	String safe;
	for (int i = 0; i < project_name.length(); i++) {
		const char32_t c = project_name[i];
		safe += (is_ascii_alphanumeric_char(c) || c == '-' || c == '_') ? String::chr(c) : String("-");
	}
	return "fast." + safe.to_lower();
}

static void _apply_default_bundle_identifier(const Ref<EditorExportPreset> &p_preset) {
	if (p_preset.is_null()) {
		return;
	}
	if (!String(p_preset->get("application/bundle_identifier")).strip_edges().is_empty()) {
		return;
	}
	const String identifier = _default_bundle_identifier();
	p_preset->set("application/bundle_identifier", identifier);
	print_line(vformat("Grit: set the preset's bundle identifier to \"%s\"; change it in the preset options before shipping.", identifier));
}

// "macOS Fast" is the platform that compiles the project into the export. The
// ordinary "macOS" platform stays plain Godot and ships GDScript.
// A Fast template is a full engine build for the target platform, which only works
// when that platform is the one we are running on.
static bool _is_host_platform(const String &p_os_name) {
	const String host = OS::get_singleton()->get_name();
	return p_os_name == host || (host == "macOS" && p_os_name == "macOS");
}

static bool _is_fast_platform(const Ref<EditorExportPlatform> &p_platform) {
	return p_platform.is_valid() && p_platform->get_name().ends_with(" Fast");
}

void GritExportPlugin::_get_export_options(const Ref<EditorExportPlatform> &p_export_platform, List<EditorExportPlatform::ExportOption> *r_options) const {
	last_platform = p_export_platform;

	// Give a macOS preset with no output path a sensible default: an .app bundle.
	// Other extensions are still accepted, this only fills an empty field.
	Ref<EditorExportPreset> preset = get_export_preset();
	if (preset.is_valid() && preset->get_export_path().is_empty() && p_export_platform.is_valid() && p_export_platform->get_os_name() == "macOS") {
		String name = GLOBAL_GET("application/config/name");
		if (name.is_empty()) {
			name = "project";
		}
		String safe;
		for (int i = 0; i < name.length(); i++) {
			const char32_t c = name[i];
			safe += (is_ascii_alphanumeric_char(c) || c == '-' || c == '_') ? String::chr(c) : String("_");
		}
		preset->set_export_path("res://build/" + safe + ".app");
	}
	// The VM/Compiled choice lives in the editor toolbar (see editor_node.cpp),
	// and debug vs release comes from the export dialog's "Export With Debug".
	// Nothing to add to the preset panel.
	// No building here: _get_export_options also runs while presets are loaded at
	// startup, and building there meant opening the editor kicked off a build.
	// Templates are prepared when the user adds the platform (export_platform_added)
	// and, as a fallback, just before an export.
	//
	// But a "macOS Fast" preset does need to point at its built templates, or the
	// platform check demands the stock macos.zip and reports "No export template
	// found". Record whatever already exists; build nothing.
	if (preset.is_valid() && p_export_platform.is_valid() && p_export_platform->get_os_name() == "macOS") {
		_apply_default_bundle_identifier(preset);
	}
	if (preset.is_valid() && p_export_platform.is_valid() && _is_fast_platform(p_export_platform)) {
		for (int i = 0; i < 2; i++) {
			const bool debug = i == 1;
			const String key = debug ? "custom_template/debug" : "custom_template/release";
			const String path = _compiled_template_path(debug);
			if (!path.is_empty() && FileAccess::exists(path) && String(preset->get(key)) != path) {
				preset->set(key, path);
			}
		}
	}
}

// Builds an export template that contains the project's generated code, by
// running tools/grit_build_template.py inside the engine source tree. That tree
// must already contain the built engine objects (bin/obj) for the requested
// target; the script only compiles the generated sources and relinks.
String GritExportPlugin::_resolve_engine_root() const {
	if (!resolved_engine_root.is_empty()) {
		return resolved_engine_root;
	}
	// The source folder chosen in Editor Settings > Grit > Settings.
	const String configured = GritProvisioner::configured_source();
	if (!configured.is_empty()) {
		resolved_engine_root = configured;
		return resolved_engine_root;
	}

	// Otherwise find the tree this editor was built from by walking up from the
	// executable. A staged app sits at <engine>/out/editor-staging/..., so this
	// covers both the tree binary and a staged bundle.
	String dir = OS::get_singleton()->get_executable_path().get_base_dir();
	for (int i = 0; i < 8 && !dir.is_empty(); i++) {
		if (FileAccess::exists(dir.path_join("SConstruct")) && FileAccess::exists(dir.path_join("tools/grit_build_template.py"))) {
			resolved_engine_root = dir;
			print_line(vformat("Grit: using the engine tree at \"%s\".", dir));
			return resolved_engine_root;
		}
		dir = dir.get_base_dir();
	}

	// Nothing local: fetch the source for this build's commit. A command-line export
	// never opens the export window or adds a preset, so this is its only chance.
	String fetch_error;
	const String fetched = GritProvisioner::ensure_engine(false, &fetch_error);
	if (!fetched.is_empty()) {
		resolved_engine_root = fetched;
		return resolved_engine_root;
	}
	if (!fetch_error.is_empty()) {
		ERR_PRINT("Grit: " + fetch_error);
	}
	return String();
}

void GritExportPlugin::export_platform_added(const Ref<EditorExportPreset> &p_preset) {
	if (!bool(GLOBAL_GET(SETTING_ENABLED)) || p_preset.is_null()) {
		return;
	}
	const Ref<EditorExportPlatform> platform = p_preset->get_platform();
	if (platform.is_null()) {
		return;
	}
	const String os_name = platform->get_os_name();
	if (os_name != "macOS" && os_name != "Linux" && os_name != "Windows") {
		return;
	}

	// The first preset in a project is when the engine source is fetched, so the
	// first export does not start with a silent download.
	String engine_error;
	if (GritProvisioner::ensure_engine(true, &engine_error).is_empty()) {
		ERR_PRINT("Grit: " + engine_error);
		return;
	}

	if (os_name == "macOS") {
		_apply_default_bundle_identifier(p_preset);
		// This engine's templates are arm64 (the tree is built with arch=arm64). A new
		// preset defaults to "universal", which would ask for a binary no template
		// here contains, so pick the architecture that can be built.
		if (String(p_preset->get("binary_format/architecture")) != "arm64") {
			p_preset->set("binary_format/architecture", "arm64");
			print_line("Grit: set this preset's architecture to arm64, because templates are built with arch=arm64.");
		}
	}

	// Every platform here is a Fast platform, and each is only registered on its own
	// host, so the template can always be built natively. Build both variants now so
	// the first export does not spend minutes compiling.
	for (int i = 0; i < 2; i++) {
		const bool debug = i == 1;
		String expected = _compiled_template_path(debug);
		if (!_template_needs_rebuild(expected)) {
			print_line(vformat("Grit: the %s compiled template already exists and is newer than the project's scripts; not rebuilding it.", debug ? "debug" : "release"));
			continue;
		}
		print_line(vformat("Grit: building the %s compiled template for this project.", debug ? "debug" : "release"));
		String built;
		if (_generate_and_build(debug, built) != OK) {
			ERR_PRINT(vformat("Grit: the %s compiled template could not be built; the export will report this again.", debug ? "debug" : "release"));
		}
	}
}

void GritExportPlugin::export_dialog_opened() {
	// Check (and provision) the build tools the first time the export window is
	// opened, rather than on every editor start.
	GritProvisioner::run_probe();
}

void GritExportPlugin::export_platform_removed(const Ref<EditorExportPreset> &p_preset) {
	if (!bool(GLOBAL_GET(SETTING_ENABLED)) || p_preset.is_null()) {
		return;
	}
	const Ref<EditorExportPlatform> platform = p_preset->get_platform();
	if (platform.is_null() || platform->get_os_name() != "macOS") {
		return;
	}
	print_line("Grit: export preset removed; cleaning up its templates.");

	// Every template this fork builds lives in the project's exports directory, so a
	// deleted preset takes its own with it. Only the shared, global locations that
	// older layouts used are listed as legacy candidates.
	Vector<String> candidates;
	candidates.push_back(_compiled_template_path(false));
	// Older layouts kept the template elsewhere; remove those too if they exist.
	candidates.push_back(EditorPaths::get_singleton()->get_export_templates_dir().path_join(GODOT_VERSION_FULL_CONFIG).path_join("macos_fast.zip"));
	const String engine_root = _resolve_engine_root();
	if (!engine_root.is_empty()) {
		const String safe = _safe_project_name();
		candidates.push_back(engine_root.path_join("bin/grit_templates").path_join(safe + "_release_compiled.zip"));
		candidates.push_back(engine_root.path_join("bin/grit_templates").path_join(safe + "_debug_compiled.zip"));
	}
	for (const String &path : candidates) {
		if (path.is_empty() || !FileAccess::exists(path)) {
			continue;
		}
		if (DirAccess::remove_absolute(path) == OK) {
			print_line("Grit: removed the export template " + path);
		} else {
			ERR_PRINT("Grit: could not remove the export template " + path);
		}
	}
}



Error GritExportPlugin::_build_template(const String &p_generated_dir, bool p_debug, String &r_template_path) {
	const String engine_root = _resolve_engine_root();
	if (engine_root.is_empty()) {
		ERR_PRINT(vformat("Grit: could not find the Fast_Godot source tree from \"%s\". Set the project setting \"%s\" to it (Project Settings > Grit), then export again.", OS::get_singleton()->get_executable_path(), String(SETTING_ENGINE_ROOT)));
		return ERR_UNCONFIGURED;
	}
	const String script = engine_root.path_join("tools/grit_build_template.py");
	if (!FileAccess::exists(script)) {
		ERR_PRINT(vformat("Grit: the engine tree at \"%s\" has no tools/grit_build_template.py. It is older than this editor; use the current toolchain bundle or checkout.", engine_root));
		return ERR_FILE_NOT_FOUND;
	}
	if (!FileAccess::exists(script)) {
		ERR_PRINT("Grit: build helper not found: " + script);
		return ERR_FILE_NOT_FOUND;
	}
	// The template belongs to the project (<project>/exports), under whichever name the
	// target platform resolves. Nothing is written into the preset's custom_template
	// fields, and stock templates are never touched.
	const String out_dir = ProjectSettings::get_singleton()->globalize_path("res://exports");
	DirAccess::make_dir_recursive_absolute(out_dir);
	const String final_path = _compiled_template_path(p_debug);
	{
		const Ref<EditorExportPreset> out_preset = get_export_preset();
		const Ref<EditorExportPlatform> out_platform = out_preset.is_valid() ? out_preset->get_platform() : Ref<EditorExportPlatform>();
		const bool macos_target = out_platform.is_valid() && out_platform->get_os_name() == "macOS";
		r_template_path = macos_target ? final_path.get_basename() : final_path;
	}

	List<String> args;
	args.push_back(engine_root);
	args.push_back(p_generated_dir);
	args.push_back(p_debug ? "debug" : "release");
	args.push_back(r_template_path);
	Ref<EditorExportPreset> build_preset = get_export_preset();
	Ref<EditorExportPlatform> platform = build_preset.is_valid() ? build_preset->get_platform() : Ref<EditorExportPlatform>();
	const String platform_name = platform.is_valid() ? platform->get_name() : String();
		String plat_id = platform_name.replace(" Fast", "");
	if (plat_id == "macOS") {
		plat_id = "macos";
	} else if (plat_id == "Linux") {
		plat_id = "linuxbsd";
	} else if (plat_id == "Windows") {
		plat_id = "windows";
	} else {
		plat_id = plat_id.to_lower();
	}
	// Arch for a *native* build of this platform. Nothing is cross-compiled: a Fast
	// template is only ever built for the platform the editor runs on.
	String arch = "arm64";
	if (plat_id != "macos") {
		const Ref<EditorExportPreset> arch_preset = get_export_preset();
		arch = arch_preset.is_valid() ? String(arch_preset->get("binary_format/architecture")) : String();
		if (arch.is_empty()) {
			arch = "x86_64";
		}
	}

	args.push_back(plat_id);

	// The template is always a real SCons build in an isolated copy (see
	// grit_build_template.sh): Grit is only compiled into a template when
	// grit_generated points at this project's sources, so there is no Grit-free
	// binary to relink against. A project without a profile gets the default
	// module set, written out as an empty profile. Hand it over as a file:
	// OS::execute does not inherit the editor's environment.
	String profile_json;
	if (ProjectSettings::get_singleton()->has_setting("grit/build_profile_json")) {
		profile_json = String(ProjectSettings::get_singleton()->get_setting("grit/build_profile_json"));
	}
	if (profile_json.is_empty()) {
		profile_json = "{\n\t\"type\": \"build_profile\"\n}\n";
	}
	String build_profile = p_generated_dir.path_join("build_profile.json");
	{
		Ref<FileAccess> profile_file = FileAccess::open(build_profile, FileAccess::WRITE);
		if (profile_file.is_valid()) {
			profile_file->store_string(profile_json);
			profile_file->close();
		} else {
			build_profile = String();
			ERR_PRINT("Grit: could not write the build profile next to the generated sources.");
			return ERR_CANT_CREATE;
		}
	}
	args.push_back(build_profile);
	args.push_back(arch);

	print_line("Grit: building the compiled template (compiles the generated code and links it).");

	// Run the build as a child process and follow its log, so the editor stays
	// responsive and the modal progress reports what is happening.
	// The helper writes build.log. It must not be last_build.log: this loop keeps a
	// read handle on the file it tails, and on Windows that denies the helper's write,
	// which failed every editor-driven build with a bare exit 1.
	const String log_path = engine_root.path_join("bin/grit").path_join(p_debug ? "debug" : "release").path_join("build.log");
	// Empty the log before spawning. The helper truncates it as well, but the poll
	// below can run before it does, and would then show the previous build's last
	// line as if it belonged to this one.
	{
		Ref<FileAccess> stale_log = FileAccess::open(log_path, FileAccess::WRITE);
		if (stale_log.is_valid()) {
			stale_log->close();
		}
	}
	// The helper is Python: the toolchain bundle already ships an interpreter, and
	// this way no platform needs a shell, rsync or coreutils.
	const String python = GritProvisioner::python_for_build();
	if (python.is_empty()) {
		ERR_PRINT("Grit: no Python interpreter found to run the build helper.");
		return ERR_CANT_CREATE;
	}
	GritProvisioner::write_build_env(engine_root);
	ProcessID pid = 0;
	List<String> python_args;
	python_args.push_back(script);
	for (const String &arg : args) {
		python_args.push_back(arg);
	}
	const Error spawn = OS::get_singleton()->create_process(python, python_args, &pid);
	if (spawn != OK) {
		ERR_PRINT("Grit: could not start the build helper " + script + " through " + python);
		return ERR_CANT_CREATE;
	}

	// No cancel button: a click needs a GUI frame, and this export is synchronous.
	// Esc is polled instead, which only needs the event queue to be pumped.
	EditorProgress progress("grit_build", TTR("Building compiled export (Esc to cancel)"), 100, false);
	int shown_percent = -1;
	String shown_line;
	int lines_seen = 0;
	bool cancel_requested = false;
	while (OS::get_singleton()->is_process_running(pid)) {
		String current_line;
		Ref<FileAccess> log_file = FileAccess::open(log_path, FileAccess::READ);
		if (log_file.is_valid()) {
			const PackedStringArray lines = log_file->get_as_text().split("\n");
			lines_seen = lines.size();
			for (int i = lines.size() - 1; i >= 0; i--) {
				const String line = lines[i].strip_edges();
				if (!line.is_empty()) {
					current_line = line.left(120);
					break;
				}
			}
		}
		// SCons does not report a percentage here, so advance on log activity:
		// monotonic, capped below 100 until the process actually exits.
		const int percent = CLAMP(5 + lines_seen / 20, 5, 95);
		const bool changed = percent != shown_percent || current_line != shown_line;
		// The export runs on the main thread and blocks it, so nothing else
		// dispatches input events. Without pumping here no key event ever reaches
		// Input, and Esc is only noticed if it happens to be held across a
		// repaint — which is why a tap appeared to need two presses.
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->process_events();
		}
		// Ask the helper to cancel: it owns its scons child, so we never have to
		// signal another process (which Godot's process bookkeeping rejects).
		if (Input::get_singleton()->is_key_pressed(Key::ESCAPE) && !cancel_requested) {
			Ref<FileAccess> cf = FileAccess::open(log_path.get_base_dir().path_join("cancel_build"), FileAccess::WRITE);
			if (cf.is_valid()) {
				cf->store_string("1");
				cf->close();
			}
			cancel_requested = true;
			print_line("Grit: cancelling the build...");
		}
		progress.step(current_line, percent, changed);
		shown_percent = percent;
		shown_line = current_line;
		// Poll often enough not to miss a key tap.
		OS::get_singleton()->delay_usec(30000);
	}
	const int exit_code = OS::get_singleton()->get_process_exit_code(pid);
	if (cancel_requested || exit_code == 3) {
		ERR_PRINT("Grit: build cancelled.");
		return ERR_SKIP;
	}
	if (exit_code != 0) {
		ERR_PRINT("Grit: template build failed (exit " + itos(exit_code) + ") running \"" + python + "\" \"" + script + "\" for engine \"" + engine_root + "\".");
		Ref<FileAccess> log_file = FileAccess::open(log_path, FileAccess::READ);
		if (log_file.is_valid()) {
			ERR_PRINT(log_file->get_as_text().right(2000));
		}
		return ERR_CANT_CREATE;
	}
	if (platform_name.begins_with("macOS")) {
		r_template_path += ".zip";
	}
	print_line("Grit: compiled executable written to " + r_template_path + ".");
	return OK;
}

// Code generation plus the template build. Runs from prepare_export() before the
// platform starts, so a failure can stop the export cleanly instead of producing
// a VM app; _export_begin() calls it too for export paths that do not prepare.
Error GritExportPlugin::_generate_and_build(bool p_debug, String &r_template_path) {
	// The export window's "Strip Scripts" checkbox wins; the project setting is the
	// fallback for command-line exports and "Export All".
	bool strip_requested = bool(ProjectSettings::get_singleton()->get_setting(OPTION_STRIP_SCRIPTS, false));
	if (EditorSettings *editor_settings = EditorSettings::get_singleton()) {
		// A non-BOOL default means "not stored by the export window": no error, and
		// the project setting above stays in charge.
		const Variant stored = editor_settings->get_project_metadata("export_options", "strip_scripts", String());
		if (stored.get_type() == Variant::BOOL) {
			strip_requested = bool(stored);
		}
	}
	const bool strip = strip_requested && !p_debug;
	if (strip_requested && p_debug) {
		print_line("Grit: scripts are only stripped in release exports, this debug export keeps them.");
	}

	const String output_directory = ProjectSettings::get_singleton()->globalize_path(GLOBAL_GET(SETTING_GENERATED_DIRECTORY));
	GritProjectCompiler compiler;
	GritProjectCompiler::Report report;
	const Error error = compiler.compile(output_directory, strip, report);
	if (error != OK) {
		ERR_PRINT("Grit: native code generation failed.");
		return error;
	}

	stripped_sources = report.stripped_sources;
	const PackedByteArray pack_fingerprint = GritRegistry::make_pack_fingerprint(report.fingerprint, strip).to_utf8_buffer();
	customization_hash = GritRegistry::make_pack_fingerprint(report.fingerprint, strip).hash64();
	prepared_fingerprint = pack_fingerprint;
	print_line(vformat("Grit: generated C++ written to \"%s\".", output_directory));

	return _build_template(output_directory, p_debug, r_template_path);
}

// Newest modification time of anything that can affect the generated code, so a
// template built before the last script edit is treated as stale.
static void _newest_project_source(const String &p_dir, uint64_t &r_newest) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name.begins_with(".")) {
			continue;
		}
		const String path = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			_newest_project_source(path, r_newest);
		} else if (name.ends_with(".gd") || name.ends_with(".tscn") || name.ends_with(".tres") || name.ends_with(".cs")) {
			const uint64_t modified = FileAccess::get_modified_time(path);
			if (modified > r_newest) {
				r_newest = modified;
			}
		}
	}
	dir->list_dir_end();
}

bool GritExportPlugin::_template_needs_rebuild(const String &p_template_path) const {
	if (p_template_path.is_empty() || !FileAccess::exists(p_template_path)) {
		return true;
	}
	uint64_t newest = 0;
	_newest_project_source("res://", newest);
	return newest > FileAccess::get_modified_time(p_template_path);
}

// Fast template file name for a target platform, matching what its Fast export
// platform resolves.
static String _fast_template_name(const String &p_os, bool p_debug, const String &p_arch) {
	const String target = p_debug ? "debug" : "release";
	if (p_os == "Windows") {
		return "windows_fast_" + target + "_" + p_arch + ".exe";
	}
	if (p_os == "Linux") {
		return "linux_fast_" + target + "." + p_arch;
	}
	return "macos_fast.zip";
}

String GritExportPlugin::_compiled_template_path(bool p_debug) const {
	const Ref<EditorExportPreset> preset = get_export_preset();
	const Ref<EditorExportPlatform> platform = preset.is_valid() ? preset->get_platform() : Ref<EditorExportPlatform>();
	if (platform.is_valid() && platform->get_os_name() != "macOS") {
		// Other platforms take one binary per target, named the way their platform
		// expects (windows_fast_release_x86_64.exe, linux_fast_release.x86_64).
		String arch = preset.is_valid() ? String(preset->get("binary_format/architecture")) : String();
		if (arch.is_empty()) {
			arch = "x86_64";
		}
		return ProjectSettings::get_singleton()->globalize_path("res://exports").path_join(_fast_template_name(platform->get_os_name(), p_debug, arch));
	}
	// macOS packages a single archive for both variants.
	return EditorExportPlatformMacOSFast::template_path();
}

Error GritExportPlugin::_prepare_compiled(const Ref<EditorExportPreset> &p_preset, bool p_debug) {
	script_mode = p_preset.is_valid() ? p_preset->get_script_export_mode() : EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED;

	String template_path;
	const Error build_error = _generate_and_build(p_debug, template_path);
	if (build_error != OK) {
		return build_error;
	}

	// Deliberately nothing is written into the preset: the platform finds its
	// template in the default templates directory by name.
	(void)template_path;
	prepared_compiled = true;
	prepared_debug = p_debug;
	return OK;
}

Error GritExportPlugin::prepare_export(const Ref<EditorExportPreset> &p_preset, bool p_debug, const String &p_path) {
	if (!bool(GLOBAL_GET(SETTING_ENABLED))) {
		return OK;
	}
	const Ref<EditorExportPreset> platform_preset = get_export_preset();
	if (platform_preset.is_valid() && platform_preset->get_platform().is_valid() && platform_preset->get_platform()->get_os_name() == "macOS") {
		_apply_default_bundle_identifier(platform_preset);
	}

	if (platform_preset.is_valid() && platform_preset->get_platform().is_valid() && _is_fast_platform(platform_preset->get_platform()) && !_is_host_platform(platform_preset->get_platform()->get_os_name())) {
		ERR_PRINT(vformat("Grit: \"%s\" templates are built on a %s host; this build does not cross-compile. Export for that platform from its own editor, or add the stock \"%s\" platform instead.",
				platform_preset->get_platform()->get_name(),
				platform_preset->get_platform()->get_os_name(),
				platform_preset->get_platform()->get_os_name()));
		return ERR_UNAVAILABLE;
	}
	if (!_is_fast_platform(platform_preset.is_valid() ? platform_preset->get_platform() : Ref<EditorExportPlatform>())) {
		// Only Fast platforms are registered, so this is a preset for a platform that
		// does not exist here.
		return OK;
	}
	const String managed_templates = String(GLOBAL_GET(SETTING_ENGINE_ROOT)).path_join("bin/grit_templates");
	_apply_default_bundle_identifier(p_preset);
	if (p_preset.is_valid()) {
		for (const char *key : { "custom_template/debug", "custom_template/release" }) {
			if (String(p_preset->get(key)).begins_with(managed_templates)) {
				p_preset->set(key, "");
			}
		}
	}
	const Error error = _prepare_compiled(p_preset, p_debug);
	if (error != OK) {
		ERR_PRINT("Grit: compiled build did not complete; the export is cancelled and nothing was written.");
	}
	return error;
}

void GritExportPlugin::_export_begin(const HashSet<String> &p_features, bool p_debug, const String &p_path, int p_flags) {
	if (!bool(GLOBAL_GET(SETTING_ENABLED))) {
		return;
	}

	const Ref<EditorExportPreset> preset = get_export_preset();
	const bool compiled = _is_fast_platform(preset.is_valid() ? preset->get_platform() : Ref<EditorExportPlatform>());

	// VM mode: stock export, no code generation at all. Drop a compiled template an
	// older build left behind so the stock template is genuinely what runs.
	if (!compiled) {
		stripped_sources.clear();
		const String managed_templates = String(GLOBAL_GET(SETTING_ENGINE_ROOT)).path_join("bin/grit_templates");
		if (preset.is_valid()) {
			for (const char *key : { "custom_template/debug", "custom_template/release" }) {
				if (String(preset->get(key)).begins_with(managed_templates)) {
					preset->set(key, "");
					WARN_PRINT(vformat("Grit: \"%s\" pointed at a compiled template left by an earlier export; using the stock template.", key));
				}
			}
		}
		return;
	}

	// The normal path: prepare_export() already generated the code and built the
	// template, and the platform is about to pack with it.
	if (prepared_compiled && prepared_debug == p_debug) {
		add_file(GritRegistry::get_fingerprint_path(), prepared_fingerprint, false);
		return;
	}

	// Export paths that never call prepare_export() (they bypass the editor's
	// dialog). Do the work here and abandon the export if it fails.
	stripped_sources.clear();
	const Error error = _prepare_compiled(preset, p_debug);
	if (error != OK) {
		if (preset.is_valid()) {
			const String managed_templates = String(GLOBAL_GET(SETTING_ENGINE_ROOT)).path_join("bin/grit_templates");
			preset->set(p_debug ? "custom_template/debug" : "custom_template/release", managed_templates.path_join(".build-did-not-complete"));
			ERR_PRINT("Grit: compiled build did not complete; the export is abandoned. Run it again (or switch the toolbar to VM for a normal export).");
		}
		return;
	}
	add_file(GritRegistry::get_fingerprint_path(), prepared_fingerprint, false);
}

void GritExportPlugin::_export_end() {
	// Put the preset back exactly as it was found: a compiled export must not
	// change how a later VM export behaves, and the compiled template path must
	// never survive in export_presets.cfg.
	if (templates_saved) {
		const Ref<EditorExportPreset> preset = get_export_preset();
		if (preset.is_valid()) {
			preset->set("custom_template/debug", previous_template_debug);
			preset->set("custom_template/release", previous_template_release);
		}
		templates_saved = false;
		previous_template_debug = String();
		previous_template_release = String();
	}
	prepared_compiled = false;
	prepared_debug = false;
	prepared_fingerprint = PackedByteArray();
	stripped_sources.clear();
}

void GritExportPlugin::_export_file(const String &p_path, const String &p_type, const HashSet<String> &p_features) {
	const String *source = stripped_sources.getptr(p_path);
	if (!source) {
		return;
	}
	if (script_mode == EditorExportPreset::MODE_SCRIPT_TEXT) {
		add_file(p_path, source->to_utf8_buffer(), false);
		skip();
		return;
	}
	const GDScriptTokenizerBuffer::CompressMode compress_mode = script_mode == EditorExportPreset::MODE_SCRIPT_BINARY_TOKENS_COMPRESSED ? GDScriptTokenizerBuffer::COMPRESS_ZSTD : GDScriptTokenizerBuffer::COMPRESS_NONE;
	const Vector<uint8_t> tokens = GDScriptTokenizerBuffer::parse_code_string(*source, compress_mode);
	ERR_FAIL_COND_MSG(tokens.is_empty(), vformat("Grit: could not tokenize the stripped \"%s\".", p_path));
	add_file(p_path.get_basename() + ".gdc", tokens, true);
}

bool GritExportPlugin::_begin_customize_resources(const Ref<EditorExportPlatform> &p_platform, const Vector<String> &p_features) {
	for (const KeyValue<String, String> &entry : stripped_sources) {
		if (entry.key.contains("::")) {
			return true;
		}
	}
	return false;
}

Ref<Resource> GritExportPlugin::_customize_resource(const Ref<Resource> &p_resource, const String &p_path) {
	const Ref<GDScript> script = p_resource;
	if (script.is_null() || !script->is_built_in()) {
		return Ref<Resource>();
	}
	const String *source = stripped_sources.getptr(script->get_path());
	if (!source || ResourceCache::get_ref(script->get_path()).ptr() == script.ptr()) {
		return Ref<Resource>();
	}
	script->set_source_code(*source);
	return script;
}

uint64_t GritExportPlugin::_get_customization_configuration_hash() const {
	return customization_hash;
}
