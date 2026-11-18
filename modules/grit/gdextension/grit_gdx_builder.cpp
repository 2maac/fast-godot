#include "core/version.h"
#include "grit_gdx_builder.h"

#include "grit_gdx_platform.h"

#include "grit/editor/grit_provisioner.h"

#include "core/extension/extension_api_dump.h"
#include "core/io/dir_access.h"
#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/string_builder.h"

static const char *GODOT_CPP_URL = "https://github.com/godotengine/godot-cpp";
// 10.0.0-stable is the release that targets the 4.7 GDExtension API. An older
// branch (4.5) runs fine but crashes the editor when it unloads the library on
// quit, because its teardown frees method binds the engine already freed.
static const char *GODOT_CPP_BRANCH = "10.0.0-stable";

String GritGdxBuilder::cached_root;
String GritGdxBuilder::cached_api;

void GritGdxBuilder::resolve_paths() {
	if (cached_root.is_empty()) {
		cached_root = godot_cpp_root();
	}
	if (cached_api.is_empty()) {
		cached_api = api_json_path();
	}
}

String GritGdxBuilder::godot_cpp_root() {
	if (!cached_root.is_empty()) {
		return cached_root;
	}
	const String override_root = OS::get_singleton()->get_environment("GRIT_GDX_GODOT_CPP");
	if (!override_root.is_empty()) {
		return override_root;
	}
	const String toolchain = GritProvisioner::configured_toolchain();
	if (!toolchain.is_empty() && DirAccess::dir_exists_absolute(toolchain)) {
		return toolchain.path_join("godot-cpp");
	}
	// No toolchain configured: use the folder the toolchain downloader targets,
	// so the cache lives where a toolchain would keep it.
	const String home_toolchain = GritProvisioner::home_toolchain_folder();
	if (DirAccess::dir_exists_absolute(home_toolchain)) {
		return home_toolchain.path_join("godot-cpp");
	}
	return OS::get_singleton()->get_data_path().path_join("grit-tools").path_join("godot-cpp");
}

String GritGdxBuilder::api_json_path() {
	if (!cached_api.is_empty()) {
		return cached_api;
	}
	const String root = OS::get_singleton()->get_data_path().path_join("grit-tools");
	DirAccess::make_dir_recursive_absolute(root);
	String name = "extension_api";
	const String commit = GritProvisioner::engine_commit();
	if (!commit.is_empty()) {
		name += "_" + commit.substr(0, 8);
	}
	const String path = root.path_join(name + ".json");
	if (FileAccess::exists(path)) {
		// An older editor dumped classes registered by loaded GDExtensions into this
		// file. godot-cpp cannot generate bindings for some of them (a class whose
		// method has its own name becomes a constructor with a return type), so
		// rebuild the dump instead of poisoning every later build.
		Error read_error = OK;
		const String existing = FileAccess::get_file_as_string(path, &read_error);
		const bool has_extension_classes = existing.contains("\"api_type\": \"extension\"") || existing.contains("\"api_type\": \"editor_extension\"");
		if (read_error == OK && !has_extension_classes) {
			cached_api = path;
			return path;
		}
		print_line("Grit: regenerating the extension API dump (it contained GDExtension classes).");
	}
	print_line(vformat("Grit: dumping the extension API to \"%s\".", path));
	GDExtensionAPIDump::generate_extension_json_file(path);
	cached_api = path;
	if (FileAccess::exists(path)) {
		return path;
	}
	// A read-only data directory (sandboxed editor, CI) cannot take the dump.
	// godot-cpp ships the API it was generated from; it matches this engine's
	// version, so the build can proceed with it instead of failing inside scons.
	const String fallback = godot_cpp_root().path_join("gdextension").path_join(vformat("extension_api-%d-%d.json", VERSION_MAJOR, VERSION_MINOR));
	if (FileAccess::exists(fallback)) {
		print_line(vformat("Grit: cannot write \"%s\"; using \"%s\" instead.", path, fallback));
		cached_api = fallback;
		return fallback;
	}
	print_line(vformat("Grit: could not write the extension API to \"%s\".", path));
	return path;
}



String GritGdxBuilder::static_library_path(const String &p_target) {
	return godot_cpp_root().path_join("bin").path_join(vformat("libgodot-cpp.%s.%s.%s.a", grit_gdx_platform_key(), p_target, grit_gdx_arch_key()));
}

String GritGdxBuilder::shell_quote(const String &p_text) {
	if (p_text.is_empty()) {
		return "\"\"";
	}
	if (OS::get_singleton()->get_name() == "Windows") {
		return "\"" + p_text + "\"";
	}
	String escaped = p_text.replace("'", "'\\''");
	return "'" + escaped + "'";
}

bool GritGdxBuilder::run(const String &p_executable, const List<String> &p_arguments, String &r_output, int &r_exit_code, String &r_error) {
	int exit_code = -1;
	const Error error = OS::get_singleton()->execute(p_executable, p_arguments, &r_output, &exit_code, true);
	if (error != OK) {
		r_error = vformat("could not run \"%s\"", p_executable);
		return false;
	}
	r_exit_code = exit_code;
	return true;
}

String GritGdxBuilder::find_scons_module(const String &p_toolchain) {
	if (p_toolchain.is_empty()) {
		return String();
	}
	const String scons_root = p_toolchain.path_join("scons");
	Ref<DirAccess> versions = DirAccess::open(scons_root);
	if (versions.is_null()) {
		return String();
	}
	for (const String &version : versions->get_directories()) {
		Ref<DirAccess> folder = DirAccess::open(scons_root.path_join(version));
		if (folder.is_null()) {
			continue;
		}
		for (const String &entry : folder->get_directories()) {
			const String candidate = scons_root.path_join(version).path_join(entry);
			if (FileAccess::exists(candidate.path_join("SCons").path_join("__init__.py"))) {
				return candidate;
			}
		}
	}
	return String();
}

bool GritGdxBuilder::run_scons(const String &p_directory, const List<String> &p_arguments, String &r_output, String &r_error) {
	const String scons_override = OS::get_singleton()->get_environment("GRIT_GDX_SCONS");
	if (!scons_override.is_empty()) {
		List<String> arguments;
		arguments.push_back("-C");
		arguments.push_back(p_directory);
		for (const String &argument : p_arguments) {
			arguments.push_back(argument);
		}
		int exit_code = -1;
		if (!run(scons_override, arguments, r_output, exit_code, r_error)) {
			return false;
		}
		if (exit_code != 0) {
			r_error = vformat("scons failed with exit code %d", exit_code);
			return false;
		}
		return true;
	}

	// Toolchain installs provide the SCons module, not a launcher, so it runs
	// through the bundled interpreter with PYTHONPATH pointing at it.
	const String python = GritProvisioner::python_for_build();
	const String toolchain = GritProvisioner::configured_toolchain();
	const String scons_module = find_scons_module(toolchain);
	if (python.is_empty()) {
		r_error = "no Python interpreter for the build (set a toolchain or GRIT_GDX_SCONS)";
		return false;
	}

	StringBuilder command;
	const bool is_windows = OS::get_singleton()->get_name() == "Windows";
	if (!scons_module.is_empty()) {
		command.append(is_windows ? "set PYTHONPATH=" + scons_module + "&& " : "PYTHONPATH=" + shell_quote(scons_module) + " ");
	}
	command.append(shell_quote(python) + " -m SCons -C " + shell_quote(p_directory));
	for (const String &argument : p_arguments) {
		command.append(" " + shell_quote(argument));
	}

	List<String> arguments;
	String shell;
	if (is_windows) {
		shell = "cmd";
		arguments.push_back("/c");
	} else {
		shell = "/bin/sh";
		arguments.push_back("-c");
	}
	arguments.push_back(command.as_string());

	int exit_code = -1;
	if (!run(shell, arguments, r_output, exit_code, r_error)) {
		return false;
	}
	if (exit_code != 0) {
		r_error = vformat("scons failed with exit code %d", exit_code);
		return false;
	}
	return true;
}

bool GritGdxBuilder::ensure_godot_cpp(const String &p_target, String &r_error) {
	const String root = godot_cpp_root();
	if (!FileAccess::exists(root.path_join("SConstruct"))) {
		print_line(vformat("Grit: fetching godot-cpp (%s) into \"%s\".", GODOT_CPP_BRANCH, root));
		DirAccess::make_dir_recursive_absolute(root.get_base_dir());
		List<String> arguments;
		arguments.push_back("clone");
		arguments.push_back("--depth");
		arguments.push_back("1");
		arguments.push_back("--branch");
		arguments.push_back(GODOT_CPP_BRANCH);
		arguments.push_back(GODOT_CPP_URL);
		arguments.push_back(root);
		String output;
		int exit_code = -1;
		if (!run("git", arguments, output, exit_code, r_error)) {
			r_error = "git is required to fetch godot-cpp: " + r_error;
			return false;
		}
		if (exit_code != 0) {
			r_error = "could not clone godot-cpp:\n" + output;
			return false;
		}
	}

	const String library = static_library_path(p_target);
	if (FileAccess::exists(library)) {
		return true;
	}

	const String api = api_json_path();
	print_line(vformat("Grit: building godot-cpp for %s (%s). This runs once.", p_target, grit_gdx_arch_key()));
	List<String> arguments;
	arguments.push_back("platform=" + grit_gdx_platform_key());
	arguments.push_back("target=" + p_target);
	arguments.push_back("arch=" + grit_gdx_arch_key());
	arguments.push_back("custom_api_file=" + api);
	arguments.push_back("-j" + itos(MAX(1, OS::get_singleton()->get_processor_count())));
	String output;
	if (!run_scons(root, arguments, output, r_error)) {
		r_error = r_error + "\n" + output.right(4000);
		return false;
	}
	return true;
}

static String grit_gdx_target_config(const String &p_target) {
	return (p_target == "debug" || p_target == "template_debug" || p_target == "editor") ? "debug" : "release";
}

static String grit_gdx_library_extension() {
	const String platform = grit_gdx_platform_key();
	if (platform == "macos") {
		return ".dylib";
	}
	if (platform == "windows") {
		return ".dll";
	}
	return ".so";
}

String GritGdxBuilder::library_path(const String &p_project_directory, const String &p_library_name, const String &p_target) {
	const String directory = ProjectSettings::get_singleton()->globalize_path(p_project_directory);
	return directory.path_join("bin").path_join(vformat("lib%s.%s.%s.%s%s", p_library_name, grit_gdx_platform_key(), grit_gdx_arch_key(), grit_gdx_target_config(p_target), grit_gdx_library_extension()));
}

bool GritGdxBuilder::build(const String &p_project_directory, const String &p_library_name, const String &p_target, String &r_library, String &r_error) {
	const String directory = ProjectSettings::get_singleton()->globalize_path(p_project_directory);
	if (!FileAccess::exists(directory.path_join("SConstruct"))) {
		r_error = vformat("\"%s\" is not a generated extension project", directory);
		return false;
	}
	if (!ensure_godot_cpp(p_target, r_error)) {
		return false;
	}
	const String api = api_json_path();
	if (!FileAccess::exists(api)) {
		r_error = vformat("the extension API dump is missing (\"%s\" could not be written and no godot-cpp copy is available); the editor needs a writable data directory to build an extension", api);
		return false;
	}
	List<String> arguments;
	arguments.push_back("platform=" + grit_gdx_platform_key());
	arguments.push_back("target=" + p_target);
	arguments.push_back("arch=" + grit_gdx_arch_key());
	arguments.push_back("custom_api_file=" + api);
	arguments.push_back("godot_cpp=" + godot_cpp_root());
	// Instances must be re-creatable for GDExtensionManager::reload_extension(),
	// which the editor uses after a rebuild (adding a class to an extension).
	arguments.push_back("use_hot_reload=yes");
	arguments.push_back("-j" + itos(MAX(1, OS::get_singleton()->get_processor_count())));
	const String config = grit_gdx_target_config(p_target);
	print_line(vformat("Grit: building the GDExtension library (%s, %s).", config, grit_gdx_arch_key()));
	String output;
	if (!run_scons(directory, arguments, output, r_error)) {
		r_error = r_error + "\n" + output.right(4000);
		return false;
	}
	r_library = library_path(p_project_directory, p_library_name, p_target);
	if (!FileAccess::exists(r_library)) {
		r_error = vformat("scons finished but \"%s\" was not produced", r_library);
		return false;
	}
	return true;
}
