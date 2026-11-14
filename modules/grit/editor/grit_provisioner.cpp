#include "grit_provisioner.h"

#include "core/config/project_settings.h"
#include "core/crypto/hashing_context.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "servers/display/display_server.h"

// Everything this module needs comes from two project settings:
//   grit/export/toolchain_root  an unzipped Fast_Godot_Toolchain_* folder
//   grit/export/engine_root     a local Fast_Godot checkout (offline alternative)
// The engine source is otherwise fetched for this editor's own commit, which is what
// keeps a toolchain folder from ever going stale.

void GritProvisioner::register_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	// The toolchain folder, and the source folder. Both are per machine, both start
	// empty, and neither is ever filled in by the editor unless the user asks.
	if (settings->has_setting("grit/settings/toolchain_root")) {
		const String old = String(settings->get_setting("grit/settings/toolchain_root")).strip_edges();
		if (!old.is_empty()) {
			settings->set_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT, old);
		}
		settings->erase("grit/settings/toolchain_root");
	}
	if (settings->has_setting("grit/settings/source_path")) {
		const String old = String(settings->get_setting("grit/settings/source_path")).strip_edges();
		if (!old.is_empty()) {
			settings->set_setting(GritProvisioner::SETTING_SOURCE_PATH, old);
		}
		settings->erase("grit/settings/source_path");
	}
	if (settings->has_setting("grit/toolchain/root")) {
		// Carry a value over from the key used before both lived under Grit > Settings.
		const String old = String(settings->get_setting("grit/toolchain/root")).strip_edges();
		if (!old.is_empty()) {
			settings->set_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT, old);
		}
		settings->erase("grit/toolchain/root");
	}
	EDITOR_DEF(GritProvisioner::SETTING_TOOLCHAIN_ROOT, "");
	settings->add_property_hint(PropertyInfo(Variant::STRING, GritProvisioner::SETTING_TOOLCHAIN_ROOT, PROPERTY_HINT_DIR));
	EDITOR_DEF(GritProvisioner::SETTING_SOURCE_PATH, "");
	settings->add_property_hint(PropertyInfo(Variant::STRING, GritProvisioner::SETTING_SOURCE_PATH, PROPERTY_HINT_DIR));
}

void GritProvisioner::purge_obsolete_settings() {
	EditorSettings *editor = EditorSettings::get_singleton();
	if (editor) {
		List<PropertyInfo> props;
		editor->get_property_list(&props);
		Vector<String> stale;
		for (const PropertyInfo &prop : props) {
			const String name = prop.name;
			// Anything grit-scoped that is not one of the two settings we keep.
			if (name.begins_with("grit/") &&
					name != GritProvisioner::SETTING_SOURCE_PATH &&
					name != GritProvisioner::SETTING_TOOLCHAIN_ROOT) {
				stale.push_back(name);
			}
		}
		for (const String &name : stale) {
			editor->erase(name);
		}
	}
	ProjectSettings *project = ProjectSettings::get_singleton();
	if (project) {
		List<PropertyInfo> props;
		project->get_property_list(&props);
		Vector<String> stale;
		for (const PropertyInfo &prop : props) {
			const String name = prop.name;
			if (name.begins_with("grit/provision/") || name == "grit/export/engine_root" || name == "grit/toolchain/root") {
				stale.push_back(name);
			}
		}
		for (const String &name : stale) {
			project->clear(name);
		}
		if (!stale.is_empty()) {
			project->save();
		}
	}
}

static String _source_path() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(GritProvisioner::SETTING_SOURCE_PATH)) {
		return String();
	}
	return String(settings->get_setting(GritProvisioner::SETTING_SOURCE_PATH)).strip_edges();
}

static String _toolchain_root() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT)) {
		return String();
	}
	return String(settings->get_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT)).strip_edges();
}

static bool _toolchain_usable(const String &p_root) {
	// The engine is not part of a bundle: it is fetched for this build's commit.
	return !p_root.is_empty() && FileAccess::exists(p_root.path_join("toolchain.json"));
}

// The commit this editor was built from; the engine source is fetched for exactly it.
String GritProvisioner::engine_commit() {
	const String hash = String(VERSION_HASH);
	if (hash.is_empty() || hash == "unknown") {
		return String();
	}
	return hash;
}

// The source comes from the release, as an asset named for the commit it holds.
// CI publishes it from the same commit as the editors and removes the previous one,
// so a fetch can never yield a tree that does not match this editor.
static String _release_tag() {
	return vformat("v%d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_STATUS);
}

static String _engine_archive_url(const String &p_commit) {
	return vformat("https://github.com/2maac/fast-godot/releases/download/%s/Fast_Godot_Source_%s.zip", _release_tag(), p_commit);
}

// Where a fetched engine tree lives: inside the toolchain folder when there is one,
// so the user's folder stays self-contained, otherwise in the editor's data dir.
static String _engine_root_dir() {
	const String root = _toolchain_root();
	const String parent = _toolchain_usable(root) ? root : OS::get_singleton()->get_data_path().path_join("grit-tools");
	const String commit = GritProvisioner::engine_commit();
	return parent.path_join("engine").path_join(commit.is_empty() ? String("unknown") : commit);
}

static bool _engine_present(const String &p_dir) {
	return FileAccess::exists(p_dir.path_join("SConstruct")) &&
			FileAccess::exists(p_dir.path_join("tools").path_join("grit_build_template.py"));
}

static bool _download(const String &p_url, const String &p_dest, String &r_error) {
	List<String> args;
	args.push_back("-L");
	args.push_back("--fail");
	args.push_back("--retry");
	args.push_back("2");
	args.push_back("--connect-timeout");
	args.push_back("30");
	args.push_back("-o");
	args.push_back(p_dest);
	args.push_back(p_url);
	String output;
	int code = -1;
	OS::get_singleton()->execute("curl", args, &output, &code);
	if (code != 0) {
		r_error = vformat("curl exit %d: %s", code, output.strip_edges().right(300));
		return false;
	}
	if (!FileAccess::exists(p_dest)) {
		r_error = "no file was written";
		return false;
	}
	return true;
}

// tar handles .tar and (via bsdtar) .zip on Windows and macOS; it is the fallback
// when the interpreter cannot unpack.
static bool _extract(const String &p_archive, const String &p_dest, String &r_error) {
	DirAccess::make_dir_recursive_absolute(p_dest);
	List<String> args;
	args.push_back("-xf");
	args.push_back(p_archive);
	args.push_back("-C");
	args.push_back(p_dest);
	String output;
	int code = -1;
	OS::get_singleton()->execute("tar", args, &output, &code);
	if (code != 0) {
		r_error = vformat("tar exit %d: %s", code, output.strip_edges().right(200));
		return false;
	}
	return true;
}

// Content-Length of a URL, or 0 when the server does not say.
static int64_t _remote_size(const String &p_url) {
	List<String> args;
	args.push_back("-sIL");
	args.push_back("--max-time");
	args.push_back("30");
	args.push_back(p_url);
	String output;
	int code = -1;
	OS::get_singleton()->execute("curl", args, &output, &code);
	if (code != 0) {
		return 0;
	}
	int64_t size = 0;
	for (const String &line : output.split("\n")) {
		if (line.to_lower().begins_with("content-length:")) {
			size = line.split(":")[1].strip_edges().to_int();
		}
	}
	return size;
}

// Downloads with a progress dialog that Esc can cancel. The partial file is removed
// either way, so a cancelled download never leaves something that looks installed.
static bool _download_engine_archive(const String &p_url, const String &p_dest, String &r_error) {
	const int64_t total = _remote_size(p_url);
	EditorProgress progress("grit_engine_download", TTR("Downloading the Fast_Godot engine source"), 100, true);

	List<String> args;
	args.push_back("-L");
	args.push_back("--fail");
	args.push_back("--retry");
	args.push_back("2");
	args.push_back("--connect-timeout");
	args.push_back("30");
	args.push_back("-o");
	args.push_back(p_dest);
	args.push_back(p_url);
	ProcessID pid = 0;
	if (OS::get_singleton()->create_process("curl", args, &pid) != OK) {
		r_error = "could not start curl";
		return false;
	}
	const String label = total > 0 ? TTR("Downloading the engine source") : TTR("Downloading the engine source (size unknown)");
	while (true) {
		OS::get_singleton()->delay_usec(150000);
		int64_t downloaded = 0;
		Ref<FileAccess> partial = FileAccess::open(p_dest, FileAccess::READ);
		if (partial.is_valid()) {
			downloaded = partial->get_length();
			partial->close();
		}
		const int percent = total > 0 ? int(downloaded * 100 / total) : -1;
		const String state = total > 0
				? vformat("%s — %d of %d MB", label, downloaded / (1024 * 1024), total / (1024 * 1024))
				: vformat("%s — %d MB", label, downloaded / (1024 * 1024));
		// step() returns true when the user cancelled, not when the step succeeded.
		if (progress.step(state, percent)) {
			OS::get_singleton()->kill(pid);
			DirAccess::remove_absolute(p_dest);
			r_error = "the download was cancelled";
			return false;
		}
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->process_events();
		}
		if (!OS::get_singleton()->is_process_running(pid)) {
			break;
		}
	}
	Ref<FileAccess> done = FileAccess::open(p_dest, FileAccess::READ);
	if (done.is_null() || done->get_length() == 0) {
		DirAccess::remove_absolute(p_dest);
		r_error = "the download produced no data";
		return false;
	}
	done->close();
	return true;
}

// Unpacks p_archive into p_dest and returns the tree inside it, or empty.
static String _unpack_engine(const String &p_archive, const String &p_dest, const String &p_url, String &r_error) {
	DirAccess::remove_absolute(p_dest);
	DirAccess::make_dir_recursive_absolute(p_dest);

	bool unpacked = false;
	String unpack_error;
	const String python = GritProvisioner::python_for_build();
	if (!python.is_empty()) {
		List<String> args;
		args.push_back("-m");
		args.push_back("zipfile");
		args.push_back("-e");
		args.push_back(p_archive);
		args.push_back(p_dest);
		int code = -1;
		String output;
		const Error ran = OS::get_singleton()->execute(python, args, &output, &code);
		unpacked = ran == OK && code == 0;
		if (!unpacked) {
			unpack_error = vformat("python %s exited %d: %s", python, code, output.strip_edges().right(160));
		}
	}
	if (!unpacked) {
		String tar_error;
		if (_extract(p_archive, p_dest, tar_error)) {
			unpacked = true;
		} else {
			unpack_error += (unpack_error.is_empty() ? "" : "; ") + tar_error;
		}
	}
	if (!unpacked) {
		r_error = vformat("could not unpack the engine source downloaded from %s (%s).", p_url, unpack_error);
		return String();
	}
	// The archive extracts to a single fast-godot-<commit>/ directory.
	String tree = p_dest;
	Ref<DirAccess> dir = DirAccess::open(p_dest);
	if (dir.is_valid()) {
		dir->list_dir_begin();
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (dir->current_is_dir() && FileAccess::exists(p_dest.path_join(name).path_join("SConstruct"))) {
				tree = p_dest.path_join(name);
				break;
			}
		}
		dir->list_dir_end();
	}
	if (!_engine_present(tree)) {
		r_error = vformat("the engine source downloaded from %s is not a usable Fast_Godot tree.", p_url);
		return String();
	}
	return tree;
}

// The published toolchain bundle for the platform this editor runs on.
static String _toolchain_asset_name() {
	const String os = OS::get_singleton()->get_name();
	if (os == "Windows") {
		return "Fast_Godot_Toolchain_win64.zip";
	}
	if (os == "macOS") {
		return "Fast_Godot_Toolchain_macos_arm64.zip";
	}
	return "Fast_Godot_Toolchain_linux_x86_64.zip";
}

String GritProvisioner::fetch_toolchain_to(const String &p_folder, bool p_show_progress, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	const String folder = p_folder.strip_edges();
	if (folder.is_empty()) {
		error = "no destination folder was chosen.";
		return String();
	}
	// Something usable already in the folder is taken as-is.
	if (FileAccess::exists(folder.path_join("toolchain.json"))) {
		return folder;
	}
	const String url = vformat("https://github.com/2maac/fast-godot/releases/download/%s/%s", _release_tag(), _toolchain_asset_name());
	String archive = OS::get_singleton()->get_cache_path().path_join(_toolchain_asset_name());
	if (OS::get_singleton()->get_name() == "Windows") {
		archive = archive.replace("/", "\\");
	}
	String download_error;
	if (p_show_progress) {
		if (!_download_engine_archive(url, archive, download_error)) {
			error = vformat("could not download the toolchain from %s (%s).", url, download_error);
			return String();
		}
	} else if (!_download(url, archive, download_error)) {
		error = vformat("could not download the toolchain from %s (%s).", url, download_error);
		return String();
	}

	DirAccess::make_dir_recursive_absolute(folder);
	bool unpacked = false;
	String unpack_error;
	const String python = python_for_build();
	if (!python.is_empty()) {
		List<String> args;
		args.push_back("-m");
		args.push_back("zipfile");
		args.push_back("-e");
		args.push_back(archive);
		args.push_back(folder);
		int code = -1;
		String output;
		const Error ran = OS::get_singleton()->execute(python, args, &output, &code);
		unpacked = ran == OK && code == 0;
		if (!unpacked) {
			unpack_error = vformat("python exited %d: %s", code, output.strip_edges().right(160));
		}
	}
	if (!unpacked) {
		String tar_error;
		if (_extract(archive, folder, tar_error)) {
			unpacked = true;
		} else {
			unpack_error += (unpack_error.is_empty() ? "" : "; ") + tar_error;
		}
	}
	DirAccess::remove_absolute(archive);
	if (!unpacked) {
		error = vformat("could not unpack the toolchain downloaded from %s (%s).", url, unpack_error);
		return String();
	}
	// The archive extracts to a single Fast_Godot_Toolchain_<platform>/ directory.
	String root = folder;
	Ref<DirAccess> dir = DirAccess::open(folder);
	if (dir.is_valid()) {
		dir->list_dir_begin();
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (dir->current_is_dir() && FileAccess::exists(folder.path_join(name).path_join("toolchain.json"))) {
				root = folder.path_join(name);
				break;
			}
		}
		dir->list_dir_end();
	}
	if (!FileAccess::exists(root.path_join("toolchain.json"))) {
		error = vformat("the toolchain downloaded from %s has no toolchain.json.", url);
		return String();
	}
	return root;
}

String GritProvisioner::fetch_source_to(const String &p_folder, bool p_show_progress, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	const String folder = p_folder.strip_edges();
	if (folder.is_empty()) {
		error = "no destination folder was chosen.";
		return String();
	}
	const String commit = engine_commit();
	if (commit.is_empty()) {
		error = "this editor does not report its revision, so the source cannot be matched to it.";
		return String();
	}
	// A tree already sitting in the chosen folder is used as-is.
	if (_engine_present(folder)) {
		return folder;
	}
	const String url = _engine_archive_url(commit);
	String archive = OS::get_singleton()->get_cache_path().path_join("grit-engine-" + commit + ".zip");
	if (OS::get_singleton()->get_name() == "Windows") {
		archive = archive.replace("/", "\\");
	}
	String download_error;
	if (p_show_progress) {
		if (!_download_engine_archive(url, archive, download_error)) {
			error = vformat("could not download the engine source for %s from %s (%s).", commit, url, download_error);
			return String();
		}
	} else if (!_download(url, archive, download_error)) {
		error = vformat("could not download the engine source for %s from %s (%s).", commit, url, download_error);
		return String();
	}
	String unpack_error;
	const String tree = _unpack_engine(archive, folder, url, unpack_error);
	DirAccess::remove_absolute(archive);
	if (tree.is_empty()) {
		error = unpack_error;
	}
	return tree;
}

String GritProvisioner::configured_source() {
	const String chosen = _source_path();
	return _engine_present(chosen) ? chosen : String();
}

String GritProvisioner::ensure_engine(bool p_show_progress, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	const String configured = configured_source();
	if (!configured.is_empty()) {
		return configured;
	}
	const String existing = _engine_root_dir();
	if (_engine_present(existing)) {
		return existing;
	}

	const String commit = engine_commit();
	if (commit.is_empty()) {
		error = "this editor does not report its revision, so the engine source cannot be matched to it. Choose a source folder in Editor Settings > Grit > Settings.";
		return String();
	}
	const String url = _engine_archive_url(commit);
	String archive = OS::get_singleton()->get_cache_path().path_join("grit-engine-" + commit + ".zip");
	if (OS::get_singleton()->get_name() == "Windows") {
		archive = archive.replace("/", "\\");
	}
	print_line("Grit: downloading the engine source for " + commit + " from " + url);

	String download_error;
	if (p_show_progress) {
		if (!_download_engine_archive(url, archive, download_error)) {
			error = vformat("could not download the engine source for %s from %s (%s). If this machine has no internet access, choose a local source folder in Editor Settings > Grit > Settings instead.", commit, url, download_error);
			return String();
		}
	} else if (!_download(url, archive, download_error)) {
		error = vformat("could not download the engine source for %s from %s (%s). No internet access? Choose a local source folder in Editor Settings > Grit > Settings.", commit, url, download_error);
		return String();
	}

	const String dest = _engine_root_dir();
	DirAccess::remove_absolute(dest);
	DirAccess::make_dir_recursive_absolute(dest);

	// Unpack with Python when it works (it is portable), otherwise with tar, which
	// reads zip on Windows and macOS. One of the two always applies.
	bool unpacked = false;
	String unpack_error;
	const String python = python_for_build();
	if (!python.is_empty()) {
		List<String> args;
		args.push_back("-m");
		args.push_back("zipfile");
		args.push_back("-e");
		args.push_back(archive);
		args.push_back(dest);
		int code = -1;
		String output;
		const Error ran = OS::get_singleton()->execute(python, args, &output, &code);
		unpacked = ran == OK && code == 0;
		if (!unpacked) {
			unpack_error = vformat("python %s exited %d: %s", python, code, output.strip_edges().right(160));
		}
	}
	if (!unpacked) {
		String tar_error;
		if (_extract(archive, dest, tar_error)) {
			unpacked = true;
		} else {
			unpack_error += (unpack_error.is_empty() ? "" : "; ") + tar_error;
		}
	}
	if (!unpacked) {
		DirAccess::remove_absolute(archive);
		error = vformat("could not unpack the engine source downloaded from %s (%s).", url, unpack_error);
		return String();
	}
	DirAccess::remove_absolute(archive);

	String tree = dest;
	Ref<DirAccess> dir = DirAccess::open(dest);
	if (dir.is_valid()) {
		dir->list_dir_begin();
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (dir->current_is_dir() && FileAccess::exists(dest.path_join(name).path_join("SConstruct"))) {
				tree = dest.path_join(name);
				break;
			}
		}
		dir->list_dir_end();
	}
	if (!_engine_present(tree)) {
		error = vformat("the engine source downloaded from %s is not a usable Fast_Godot tree.", url);
		return String();
	}
	print_line("Grit: engine source for " + commit + " is in " + tree);
	return tree;
}

String GritProvisioner::python_for_build() {
	const String root = _toolchain_root();
	if (_toolchain_usable(root)) {
		const String bundled = root.path_join("python").path_join(
				OS::get_singleton()->get_name() == "Windows" ? "python.exe" : "bin/python3");
		if (FileAccess::exists(bundled)) {
			// Windows will not reliably start a program whose path uses forward
			// slashes, and this value is handed straight to create_process.
			return OS::get_singleton()->get_name() == "Windows" ? bundled.replace("/", "\\") : bundled;
		}
	}
	for (const char *candidate : { "python3", "python" }) {
		List<String> args;
		args.push_back("--version");
		String output;
		int code = -1;
		OS::get_singleton()->execute(candidate, args, &output, &code);
		if (code == 0) {
			return candidate;
		}
	}
	return String();
}

void GritProvisioner::write_build_env(const String &p_engine_root) {
	const String root = _toolchain_root();
	const String os = OS::get_singleton()->get_name();
	const bool bundled = _toolchain_usable(root);

	String python = python_for_build();
	String pythonpath;
	Vector<String> path_add;
	String args = "vulkan=yes";

	if (bundled) {
		pythonpath = root.path_join("scons").path_join("4.11.1").path_join("scons-4.11.1");
		if (os == "Windows") {
			path_add.push_back(root.path_join("mingw64").path_join("bin"));
			args = "use_mingw=yes vulkan=yes accesskit=no d3d12=no winrt=no angle=no cffi_libffi_path=" +
					root.path_join("libffi").path_join("3.8.0").path_join("mingw64");
		} else if (os == "macOS") {
			const String mvk = root.path_join("moltenvk");
			if (DirAccess::dir_exists_absolute(mvk.path_join("macOS").path_join("lib").path_join("MoltenVK.xcframework"))) {
				args += " vulkan_sdk_path=" + mvk.replace("\\", "/");
			}
		}
	}

	const String config_dir = p_engine_root.path_join("bin").path_join("grit");
	DirAccess::make_dir_recursive_absolute(config_dir);
	Ref<FileAccess> file = FileAccess::open(config_dir.path_join("build_env.json"), FileAccess::WRITE);
	if (file.is_null()) {
		return;
	}
	String json = "{\n";
	json += "  \"python\": \"" + python.replace("\\", "/") + "\",\n";
	json += "  \"pythonpath\": \"" + pythonpath.replace("\\", "/") + "\",\n";
	json += "  \"scons_args\": \"" + args.replace("\\", "/") + "\",\n";
	json += "  \"engine_id\": \"" + String(VERSION_HASH) + "\",\n";
	json += "  \"path_add\": [";
	for (int i = 0; i < path_add.size(); i++) {
		json += String(i == 0 ? "" : ", ") + "\"" + path_add[i].replace("\\", "/") + "\"";
	}
	json += "]\n}\n";
	file->store_string(json);
	file->close();
}

void GritProvisioner::run_probe() {
	static bool ran = false;
	if (ran) {
		return;
	}
	ran = true;

	String error;
	if (!ensure_engine(true, &error).is_empty()) {
		print_line("Grit: build tools ready.");
	} else {
		ERR_PRINT("Grit: " + error);
	}
}
