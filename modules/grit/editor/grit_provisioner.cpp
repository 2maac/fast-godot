#include "grit_provisioner.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/crypto/hashing_context.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/export/editor_export.h"
#include "editor/file_system/editor_paths.h"
#include "editor/editor_node.h"
#include "servers/display/display_server.h"
#include "editor/settings/editor_settings.h"

// Pinned, not "latest": a provisioner that follows a moving target eventually
// builds with a toolchain nobody tested. Checksums come from the release assets.
// SCons is a Python package, so it is downloaded into the shared tools directory
// rather than bundled in the editor. The sdist keeps script/scons.py, which is what
// the build helper needs; the wheel does not.


// One editor setting per provisioned source, so a URL that goes down can be
// redirected from the settings UI instead of a rebuild.
struct ProvisionSetting {
	const char *key;
	const char *default_value;
	const char *hint;
};

static const ProvisionSetting PROVISION_SETTINGS[] = {
	// The whole configuration. The rest of this list used to expose a source URL,
	// mirrors, a checksum and a version for every piece that could be downloaded.
	// A toolchain folder carries those pieces, and the only download left is the
	// engine source, whose URL comes from the editor's own commit, not a setting.
	{ "grit/toolchain/root", "", "Folder of an unzipped Fast_Godot_Toolchain_* archive. The editor takes Python, SCons and the platform toolchain from it and fetches the engine source for its own commit." },
};

void GritProvisioner::register_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return; // Called too early; the probe will retry.
	}
	// A changed default has to reach installs that already stored the old one.
	const int current_sources = 2;
	const String stored_sources = String(settings->get_setting("grit/provision/sources_version"));
	const bool reset_sources = stored_sources != itos(current_sources);

	for (const ProvisionSetting &entry : PROVISION_SETTINGS) {
		if (reset_sources && (String(entry.key).ends_with("/url") || String(entry.key).ends_with("/sha256") || String(entry.key).ends_with("/version"))) {
			settings->set_setting(entry.key, String(entry.default_value));
			continue;
		}
		// Empty means "use the built-in default": clearing a field restores the
		// pinned source rather than leaving the item unconfigurable.
		if (settings->has_setting(entry.key)) {
			if (String(settings->get_setting(entry.key)).strip_edges().is_empty() && String(entry.default_value) != "") {
				settings->set_setting(entry.key, String(entry.default_value));
			}
			continue;
		}
		settings->set_setting(entry.key, String(entry.default_value));
		settings->add_property_hint(PropertyInfo(Variant::STRING, entry.key, PROPERTY_HINT_PLACEHOLDER_TEXT, entry.hint));
	}
}

static String _setting(const String &p_key) {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(p_key)) {
		return String();
	}
	return String(settings->get_setting(p_key));
}


// ---------------------------------------------------------------------------
// Download and install. curl is used rather than HTTPClient: it already handles
// TLS, redirects, proxies and system certificates, and it exists on every
// platform this editor targets.
// ---------------------------------------------------------------------------

static String _mirror_list(const char *p_key) {
	return _setting(p_key);
}

static Vector<String> _sources_for(const char *p_url_key, const char *p_mirrors_key) {
	Vector<String> urls;
	const String primary = _setting(p_url_key).strip_edges();
	if (!primary.is_empty()) {
		urls.push_back(primary);
	}
	for (const String &mirror : _mirror_list(p_mirrors_key).split(",", false)) {
		const String url = mirror.strip_edges();
		if (!url.is_empty()) {
			urls.push_back(url);
		}
	}
	return urls;
}

static bool _download(const String &p_url, const String &p_dest, String &r_error) {
	List<String> args;
	args.push_back("-L");
	args.push_back("--fail");
	args.push_back("--retry");
	args.push_back("2");
	args.push_back("--connect-timeout");
	args.push_back("30");
	const String proxy = _setting("grit/provision/proxy").strip_edges();
	if (!proxy.is_empty()) {
		args.push_back("--proxy");
		args.push_back(proxy);
	}
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

static String _sha256_of(const String &p_path) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return String();
	}
	Ref<HashingContext> hashing;
	hashing.instantiate();
	hashing->start(HashingContext::HASH_SHA256);
	const int chunk = 1 << 20;
	PackedByteArray buffer;
	buffer.resize(chunk);
	while (true) {
		const int read = file->get_buffer(buffer.ptrw(), chunk);
		if (read <= 0) {
			break;
		}
		hashing->update(buffer.slice(0, read));
	}
	const Vector<uint8_t> digest = hashing->finish();
	return String::hex_encode_buffer(digest.ptr(), digest.size());
}

// tar handles .tar and (via bsdtar) .zip on every platform this targets.
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
		r_error = vformat("tar exit %d: %s", code, output.strip_edges().right(300));
		return false;
	}
	return true;
}

// True when this framework directory contains <arch>/libMoltenVK.a.
static bool _framework_has_static_lib(const String &p_framework) {
	Ref<DirAccess> dir = DirAccess::open(p_framework);
	if (dir.is_null()) {
		return false;
	}
	dir->list_dir_begin();
	bool found = false;
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (dir->current_is_dir() && FileAccess::exists(p_framework.path_join(name).path_join("libMoltenVK.a"))) {
			found = true;
			break;
		}
	}
	dir->list_dir_end();
	return found;
}

// Finds the extracted MoltenVK.xcframework at whichever depth the archive uses.
// Depth-capped and symlink-skipping: a macOS .framework contains
// Versions/Current -> A links, which would otherwise recurse forever.
static String _find_xcframework(const String &p_dir, int p_depth = 0) {
	if (p_depth > 6) {
		return String();
	}
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return String();
	}
	dir->list_dir_begin();
	Vector<String> subdirs;
	while (true) {
		const String name = dir->get_next();
		if (name.is_empty()) {
			break;
		}
		if (!dir->current_is_dir()) {
			continue;
		}
		if (name == "MoltenVK.xcframework") {
			const String found = p_dir.path_join(name);
			dir->list_dir_end();
			// The archive ships both a dynamic and a static framework; Godot links
			// the static one, so only accept a framework that has libMoltenVK.a.
			if (_framework_has_static_lib(found)) {
				return found;
			}
			return String();
		}
		if (!dir->is_link(name)) {
			subdirs.push_back(name);
		}
	}
	dir->list_dir_end();
	for (const String &name : subdirs) {
		const String found = _find_xcframework(p_dir.path_join(name), p_depth + 1);
		if (!found.is_empty()) {
			return found;
		}
	}
	return String();
}

// Installs the Khronos archive as ~/VulkanSDK/<version>/MoltenVK/MoltenVK.xcframework,
// which is one of the two layouts get_mvk_sdk_path() already recognises.
static bool _install_moltenvk(const String &p_archive, String &r_error) {
	const String temp = OS::get_singleton()->get_cache_path().path_join("grit_moltenvk_extract");
	DirAccess::remove_absolute(temp);
	if (!_extract(p_archive, temp, r_error)) {
		return false;
	}
	const String framework = _find_xcframework(temp);
	if (framework.is_empty()) {
		r_error = "MoltenVK.xcframework not found in the archive";
		return false;
	}
	const String version = _setting("grit/provision/moltenvk/version");
	const String dest = OS::get_singleton()->get_environment("HOME").path_join("VulkanSDK").path_join(version.is_empty() ? "1.4.2" : version);
	DirAccess::make_dir_recursive_absolute(dest.path_join("MoltenVK"));
	const String target = dest.path_join("MoltenVK").path_join("MoltenVK.xcframework");
	DirAccess::remove_absolute(target);
	if (DirAccess::rename_absolute(framework, target) != OK) {
		r_error = "could not move the framework into " + target;
		return false;
	}
	DirAccess::remove_absolute(temp);
	print_line("Grit: installed MoltenVK into " + dest);
	return true;
}


// The one place provisioned tools live. Everything the provisioner installs that is
// not the MoltenVK SDK (which has to sit in ~/VulkanSDK to be found) goes here.
static String _tools_dir() {
	const String configured = _setting("grit/provision/tools_dir").strip_edges();
	if (!configured.is_empty()) {
		return configured;
	}
	return EditorPaths::get_singleton()->get_data_dir().path_join("tools");
}

// The interpreter to run a downloaded SCons with. The names differ per platform:
// Windows has no python3 by default, and the py launcher is the reliable one there.
static String _python_interpreter() {
	struct Candidate {
		const char *exe;
		const char *flag;
	};
	static const Candidate candidates[] = {
		{ "python3", nullptr },
		{ "python", nullptr },
		{ "py", "-3" },
	};
	for (const Candidate &candidate : candidates) {
		List<String> args;
		if (candidate.flag) {
			args.push_back(candidate.flag);
		}
		args.push_back("--version");
		String output;
		int code = -1;
		OS::get_singleton()->execute(candidate.exe, args, &output, &code);
		if (code == 0) {
			return candidate.flag ? String(candidate.exe) + " " + candidate.flag : String(candidate.exe);
		}
	}
	return String();
}

// Downloads the SCons sdist, extracts it and drops a launcher the build helper can
// execute. scons_path.txt in the engine tree is updated so nothing else has to know.
static bool _install_scons(String &r_error) {
	const String python = _python_interpreter();
	if (python.is_empty()) {
		r_error = "no Python interpreter found; SCons needs python3 on this machine";
		return false;
	}

	const String archive = OS::get_singleton()->get_cache_path().path_join("grit_scons.tar.gz");
	print_line("Grit: downloading scons from " + _setting("grit/provision/scons/url"));
	if (!_download(_setting("grit/provision/scons/url"), archive, r_error)) {
		return false;
	}

	const String expected = _setting("grit/provision/scons/sha256").strip_edges().to_lower();
	const String actual = _sha256_of(archive).to_lower();
	if (!expected.is_empty() && actual != expected) {
		DirAccess::remove_absolute(archive);
		r_error = vformat("checksum mismatch (expected %s, got %s)", expected, actual);
		return false;
	}

	const String version = _setting("grit/provision/scons/version");
	const String dest = _tools_dir().path_join("scons").path_join(version);
	DirAccess::remove_absolute(dest);
	DirAccess::make_dir_recursive_absolute(dest);
	if (!_extract(archive, dest, r_error)) {
		DirAccess::remove_absolute(archive);
		return false;
	}
	DirAccess::remove_absolute(archive);

	// The sdist extracts as scons-<version>/. It has no script/scons.py any more:
	// the entry point is the SCons package itself, run with -m.
	const String inner = dest.path_join("scons-" + version);
	const String entry = inner.path_join("SCons").path_join("__main__.py");
	if (!FileAccess::exists(entry)) {
		r_error = "SCons/__main__.py not found in the extracted archive";
		return false;
	}

	const bool windows = OS::get_singleton()->get_name() == "Windows";
	const String launcher_dir = dest.path_join("bin");
	DirAccess::make_dir_recursive_absolute(launcher_dir);
	const String launcher = launcher_dir.path_join(windows ? "scons.cmd" : "scons");
	Ref<FileAccess> file = FileAccess::open(launcher, FileAccess::WRITE);
	if (file.is_null()) {
		r_error = "could not write " + launcher;
		return false;
	}
	if (windows) {
		file->store_line("@echo off");
		file->store_line("rem Generated by the Grit provisioner.");
		file->store_line("set PYTHONPATH=" + inner);
		file->store_line(python + " -m SCons %*");
	} else {
		file->store_line("#!/bin/sh");
		file->store_line("# Generated by the Grit provisioner.");
		file->store_line("exec env PYTHONPATH=\"" + inner + "\" " + python + " -m SCons \"$@\"");
	}
	file->close();
	if (!windows) {
		List<String> chmod_args;
		chmod_args.push_back("+x");
		chmod_args.push_back(launcher);
		OS::get_singleton()->execute("chmod", chmod_args);
	}

	// Point the build helper at it, if the engine tree is around to hold the file.
	const String engine_root = String(GLOBAL_GET("grit/export/engine_root"));
	if (!engine_root.is_empty()) {
		const String config_dir = engine_root.path_join("bin/grit");
		DirAccess::make_dir_recursive_absolute(config_dir);
		Ref<FileAccess> config = FileAccess::open(config_dir.path_join("scons_path.txt"), FileAccess::WRITE);
		if (config.is_valid()) {
			config->store_line(launcher);
			config->close();
		}
	}
	print_line("Grit: installed scons " + version + " in " + dest);
	return true;
}


// A self-contained toolchain folder, as published as a zip per platform. Setting
// grit/toolchain/root is the whole configuration: everything below is derived.
static String _toolchain_root() {
	return _setting("grit/toolchain/root").strip_edges();
}

static bool _toolchain_usable(const String &p_root) {
	if (p_root.is_empty()) {
		return false;
	}
	// The engine is not part of the bundle: it is fetched for this build's commit.
	return FileAccess::exists(p_root.path_join("toolchain.json"));
}

// The commit this editor was built from. The engine source is fetched for exactly
// this revision, which is what keeps a toolchain folder from ever going stale.
static String _engine_commit() {
	const String hash = String(VERSION_HASH);
	if (hash.is_empty() || hash == "unknown") {
		return String();
	}
	return hash;
}

static String _engine_archive_url(const String &p_commit) {
	return "https://github.com/2maac/fast-godot/archive/" + p_commit + ".zip";
}

// Where a fetched engine tree lives: inside the bundle when there is one, so the
// user's folder stays self-contained, otherwise in the editor's tools directory.
static String _engine_root_dir() {
	const String root = _toolchain_root();
	const String parent = _toolchain_usable(root) ? root : _tools_dir();
	return parent.path_join("engine").path_join(_engine_commit().is_empty() ? String("unknown") : _engine_commit());
}

static bool _engine_present(const String &p_dir) {
	return FileAccess::exists(p_dir.path_join("SConstruct")) &&
			FileAccess::exists(p_dir.path_join("tools").path_join("grit_build_template.py"));
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
	Ref<FileAccess> file;
	file = FileAccess::open(p_dest, FileAccess::READ);
	if (file.is_valid()) {
		file->close();
	}
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
		if (!progress.step(state, percent)) {
			OS::get_singleton()->kill(pid);
			DirAccess::remove_absolute(p_dest);
			r_error = "the download was cancelled";
			return false;
		}
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->process_events();
		}
		// curl removed the file on --fail before the size check, so a stalled read is
		// not mistaken for a finished download: wait for the process to disappear.
		if (OS::get_singleton()->is_process_running(pid) == false) {
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

bool GritProvisioner::ensure_engine(bool p_show_progress, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	const String existing = _engine_root_dir();
	if (_engine_present(existing)) {
		if (EditorSettings::get_singleton()) {
			EditorSettings::get_singleton()->set_setting("grit/engine_root", existing);
		}
		return true;
	}
	// A checkout the user pointed at is used as-is.
	if (EditorSettings::get_singleton()) {
		const String configured = String(EditorSettings::get_singleton()->get_setting("grit/engine_root"));
		if (_engine_present(configured)) {
			return true;
		}
	}

	const String commit = _engine_commit();
	if (commit.is_empty()) {
		error = "this editor does not report its revision, so the engine source cannot be matched to it. Set grit/export/engine_root to a local Fast_Godot checkout.";
		return false;
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
			error = vformat("could not download the engine source for %s from %s (%s). If this machine has no internet access, set grit/export/engine_root to a local Fast_Godot checkout instead.", commit, url, download_error);
			return false;
		}
	} else if (!_download(url, archive, download_error)) {
		error = vformat("could not download the engine source for %s from %s (%s). No internet access? Set grit/export/engine_root to a local Fast_Godot checkout.", commit, url, download_error);
		return false;
	}

	const String dest = _engine_root_dir();
	DirAccess::remove_absolute(dest);
	DirAccess::make_dir_recursive_absolute(dest);
	// Unpack with Python when it works (it is portable), otherwise with tar, which
	// reads zip on Windows and macOS. One of the two always applies.
	bool unpacked = false;
	String unpack_error;
	const String python = GritProvisioner::python_for_build();
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
		return false;
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
		return false;
	}
	if (EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_setting("grit/engine_root", tree);
	}
	print_line("Grit: engine source for " + commit + " is in " + tree);
	return true;
}

bool GritProvisioner::apply_toolchain_root() {
	const String root = _toolchain_root();
	if (!_toolchain_usable(root)) {
		return false;
	}
	// The engine source is fetched separately by ensure_engine(); here only a tree
	// that is already on disk is recorded.
	const String engine = _engine_root_dir();
	if (_engine_present(engine) && EditorSettings::get_singleton()) {
		EditorSettings::get_singleton()->set_setting("grit/engine_root", engine);
	}
	return true;
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
	} else {
		// Provisioned or system SCons: use the newest installed package if any.
		Ref<DirAccess> dir = DirAccess::open(_tools_dir().path_join("scons"));
		if (dir.is_valid()) {
			dir->list_dir_begin();
			String best;
			for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
				if (dir->current_is_dir() && FileAccess::exists(_tools_dir().path_join("scons").path_join(name).path_join("scons-" + name))) {
					best = name;
				}
			}
			dir->list_dir_end();
			if (!best.is_empty()) {
				pythonpath = _tools_dir().path_join("scons").path_join(best).path_join("scons-" + best);
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
	// Part of the isolated-tree key: a tree seeded by a different engine revision
	// keeps stale *.gen.* files and fails to link with symbols that no longer exist.
	json += "  \"engine_id\": \"" + String(VERSION_HASH) + "\",\n";
	json += "  \"path_add\": [";
	for (int i = 0; i < path_add.size(); i++) {
		json += String(i == 0 ? "" : ", ") + "\"" + path_add[i].replace("\\", "/") + "\"";
	}
	json += "]\n}\n";
	file->store_string(json);
	file->close();
}


bool GritProvisioner::_has_engine_tree(String &r_detail) {
	const String configured = String(GLOBAL_GET("grit/export/engine_root"));
	if (!configured.is_empty() && FileAccess::exists(configured.path_join("SConstruct"))) {
		r_detail = configured;
		return true;
	}
	// Fall back to the remembered location and the executable walk, so the check
	// agrees with what a build would actually use.
	const String remembered = _setting("grit/engine_root");
	if (!remembered.is_empty() && FileAccess::exists(remembered.path_join("SConstruct"))) {
		r_detail = remembered + " (remembered)";
		return true;
	}
	String dir = OS::get_singleton()->get_executable_path().get_base_dir();
	for (int i = 0; i < 8 && !dir.is_empty(); i++) {
		if (FileAccess::exists(dir.path_join("SConstruct")) && FileAccess::exists(dir.path_join("tools/grit_build_template.py"))) {
			r_detail = dir + " (found next to the executable)";
			return true;
		}
		dir = dir.get_base_dir();
	}
	r_detail = "set grit/export/engine_root to the Fast_Godot source tree";
	return false;
}

bool GritProvisioner::_has_scons(String &r_detail) {
	Ref<DirAccess> dir = DirAccess::open(_tools_dir().path_join("scons"));
	if (dir.is_valid()) {
		dir->list_dir_begin();
		String best;
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			const String package = _tools_dir().path_join("scons").path_join(name).path_join("scons-" + name);
			if (dir->current_is_dir() && FileAccess::exists(package.path_join("SCons").path_join("__main__.py"))) {
				best = package;
			}
		}
		dir->list_dir_end();
		if (!best.is_empty()) {
			r_detail = best;
			return true;
		}
	}
	r_detail = "not provisioned yet";
	return false;
}

bool GritProvisioner::_has_compiler(String &r_detail) {
#if defined(MACOS_ENABLED)
	String output;
	int code = -1;
	List<String> args;
	args.push_back("--find");
	args.push_back("clang");
	OS::get_singleton()->execute("xcrun", args, &output, &code);
	if (code == 0 && !output.strip_edges().is_empty()) {
		r_detail = output.strip_edges();
		return true;
	}
	r_detail = "install Xcode Command Line Tools: xcode-select --install (this one cannot be downloaded)";
	return false;
#elif defined(WINDOWS_ENABLED)
	List<String> args;
	String output;
	int code = -1;
	OS::get_singleton()->execute("x86_64-w64-mingw32-gcc", {"--version"}, &output, &code);
	if (code == 0) {
		r_detail = output.get_slice("\n", 0);
		return true;
	}
	r_detail = "install the MinGW toolchain, or point grit/provision/winlibs/url at a winlibs archive";
	return false;
#else
	String output;
	int code = -1;
	OS::get_singleton()->execute("cc", List<String>(), &output, &code);
	if (code == 0) {
		r_detail = "cc";
		return true;
	}
	r_detail = "install gcc or clang with the platform package manager";
	return false;
#endif
}

bool GritProvisioner::_has_moltenvk(String &r_detail) {
#if defined(MACOS_ENABLED)
	const String sdk = OS::get_singleton()->get_environment("VULKAN_SDK");
	if (!sdk.is_empty() && (DirAccess::dir_exists_absolute(sdk.path_join("macOS/lib/MoltenVK.xcframework")) || DirAccess::dir_exists_absolute(sdk.path_join("MoltenVK/MoltenVK.xcframework")))) {
		r_detail = sdk;
		return true;
	}
	// Newest ~/VulkanSDK/<version> that actually has MoltenVK.
	const String home = OS::get_singleton()->get_environment("HOME");
	Ref<DirAccess> dir = DirAccess::open(home.path_join("VulkanSDK"));
	if (dir.is_valid()) {
		dir->list_dir_begin();
		String best;
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			const String root = home.path_join("VulkanSDK").path_join(name);
			if (dir->current_is_dir() && (DirAccess::dir_exists_absolute(root.path_join("macOS/lib/MoltenVK.xcframework")) || DirAccess::dir_exists_absolute(root.path_join("MoltenVK/MoltenVK.xcframework")))) {
				if (name > best) {
					best = name;
				}
			}
		}
		dir->list_dir_end();
		if (!best.is_empty()) {
			r_detail = home.path_join("VulkanSDK").path_join(best);
			return true;
		}
	}
	r_detail = "download from grit/provision/moltenvk/url";
	return false;
#else
	r_detail = "not needed on this platform";
	return true;
#endif
}

Vector<GritProvisioner::Item> GritProvisioner::collect() {
	Vector<Item> items;

	// A self-contained toolchain folder satisfies everything it contains, so report
	// it with the paths the build will really use.
	const String bundle = _toolchain_root();
	if (_toolchain_usable(bundle)) {
		auto add_bundle = [&items](const char *p_id, const char *p_label, const String &p_detail) {
			Item item;
			item.id = p_id;
			item.label = p_label;
			item.state = "present";
			item.detail = p_detail;
			items.push_back(item);
		};
		const String os_name = OS::get_singleton()->get_name();
		String engine_detail;
		if (_has_engine_tree(engine_detail)) {
			add_bundle("engine_tree", "Engine source tree", engine_detail);
		} else {
			Item item;
			item.id = "engine_tree";
			item.label = "Engine source tree";
			item.state = "missing";
			item.detail = "downloadable for commit " + _engine_commit();
			items.push_back(item);
		}
		add_bundle("scons", "SCons", bundle.path_join("scons"));
		if (os_name == "Windows") {
			add_bundle("compiler", "C/C++ compiler", bundle.path_join("mingw64").path_join("bin").path_join("gcc.exe"));
			add_bundle("moltenvk", "MoltenVK", "not needed on this platform");
		} else if (os_name == "macOS") {
			// The compiler is still the system's: Apple's SDK cannot be bundled.
			add_bundle("compiler", "C/C++ compiler", "system (Xcode Command Line Tools)");
			add_bundle("moltenvk", "MoltenVK", bundle.path_join("moltenvk"));
		} else {
			add_bundle("compiler", "C/C++ compiler", "system toolchain");
			add_bundle("moltenvk", "MoltenVK", "not needed on this platform");
		}
		return items;
	}
	auto add = [&items](const String &p_id, const String &p_label, bool p_ok, const String &p_detail, bool p_manual = false) {
		Item item;
		item.id = p_id;
		item.label = p_label;
		item.state = p_ok ? "present" : (p_manual ? "manual" : "missing");
		item.detail = p_detail;
		items.push_back(item);
	};

	String detail;
	add("engine_tree", "Engine source tree", _has_engine_tree(detail), detail);
	add("scons", "SCons", _has_scons(detail), detail);
	add("compiler", "C/C++ compiler", _has_compiler(detail), detail, true);
	add("moltenvk", "MoltenVK", _has_moltenvk(detail), detail);
	return items;
}

// Downloads, verifies and installs one item. Returns true when it ended up
// present, so the caller can re-check rather than trust the installer.
static bool _install_item(const String &p_id) {
	static const struct {
		const char *id;
		const char *url_key;
		const char *mirrors_key;
		const char *sha_key;
	} sources[] = {
		{ "moltenvk", "grit/provision/moltenvk/url", "grit/provision/moltenvk/mirrors", "grit/provision/moltenvk/sha256" },
		{ "winlibs", "grit/provision/winlibs/url", "grit/provision/winlibs/mirrors", "grit/provision/winlibs/sha256" },
	};

	if (p_id == "scons") {
		String error;
		if (_install_scons(error)) {
			return true;
		}
		ERR_PRINT("Grit: scons install failed: " + error);
		return false;
	}

	const int count = sizeof(sources) / sizeof(sources[0]);
	int index = -1;
	for (int i = 0; i < count; i++) {
		if (p_id == sources[i].id) {
			index = i;
			break;
		}
	}
	if (index < 0) {
		return false; // Nothing downloadable for this item.
	}

	if (_setting("grit/provision/offline").to_lower() == "true") {
		ERR_PRINT("Grit: " + p_id + " is missing and grit/provision/offline is on; not downloading.");
		return false;
	}

	const Vector<String> urls = _sources_for(sources[index].url_key, sources[index].mirrors_key);
	if (urls.is_empty()) {
		ERR_PRINT("Grit: " + p_id + " is missing and no source is configured in " + String(sources[index].url_key) + ".");
		return false;
	}
	const String expected = _setting(sources[index].sha_key).strip_edges().to_lower();
	const bool allow_unverified = _setting("grit/provision/allow_unverified").to_lower() == "true";
	if (expected.is_empty() && !allow_unverified) {
		ERR_PRINT("Grit: " + p_id + " is missing but " + String(sources[index].sha_key) + " is empty; refusing to install an unverified download.");
		return false;
	}

	const String archive = OS::get_singleton()->get_cache_path().path_join("grit_download_" + p_id);
	for (int i = 0; i < urls.size(); i++) {
		const String &url = urls[i];
		print_line(vformat("Grit: downloading %s from %s%s", p_id, url, urls.size() > 1 ? vformat(" (%d of %d)", i + 1, urls.size()) : ""));
		String error;
		if (!_download(url, archive, error)) {
			ERR_PRINT(vformat("Grit: %s source failed: %s", p_id, error));
			continue;
		}
		const String actual = _sha256_of(archive);
		if (!expected.is_empty() && actual.to_lower() != expected) {
			ERR_PRINT(vformat("Grit: %s checksum mismatch (expected %s, got %s); the download was discarded.", p_id, expected, actual));
			DirAccess::remove_absolute(archive);
			continue;
		}
		if (!expected.is_empty()) {
			print_line("Grit: " + p_id + " checksum verified.");
		} else {
			ERR_PRINT("Grit: " + p_id + " installed WITHOUT checksum verification (allow_unverified).");
		}
		bool installed = false;
		if (p_id == "moltenvk") {
			installed = _install_moltenvk(archive, error);
		}
		DirAccess::remove_absolute(archive);
		if (installed) {
			return true;
		}
		ERR_PRINT(vformat("Grit: %s install failed: %s", p_id, error));
	}
	return false;
}


void GritProvisioner::run_probe() {
	apply_toolchain_root();
	static bool ran = false;
	if (ran) {
		return;
	}
	ran = true;

	register_settings();

	Vector<Item> items = collect();
	int missing = 0;
	for (const Item &item : items) {
		if (item.state != "present") {
			missing++;
		}
	}
	if (missing == 0) {
		for (const Item &item : items) {
			print_verbose(vformat("Grit: %s - %s", item.label, item.detail));
		}
		print_line(vformat("Grit: build tools ready (%d checks).", items.size()));
		return;
	}

	// Provisioning is not optional: no cancel button, Esc ignored, and a download
	// in progress is never interrupted by the user.
	EditorProgress progress("grit_provision", TTR("Preparing build tools"), items.size(), false);
	for (int i = 0; i < items.size(); i++) {
		if (items[i].state != "present") {
			progress.step(vformat("%s: %s", items[i].label, items[i].state), i);
			_install_item(items[i].id);
		}
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->process_events();
		}
	}

	// Report what everything looks like after the attempt, not what was attempted.
	items = collect();
	missing = 0;
	for (const Item &item : items) {
		if (item.state != "present") {
			missing++;
		}
		if (item.state == "present") {
			print_line(vformat("Grit: %s - present (%s)", item.label, item.detail));
		} else if (item.state == "manual") {
			WARN_PRINT(vformat("Grit: %s - install manually: %s", item.label, item.detail));
		} else {
			ERR_PRINT(vformat("Grit: %s - missing: %s", item.label, item.detail));
		}
	}
	if (missing == 0) {
		print_line("Grit: build tools ready.");
	} else {
		ERR_PRINT(vformat("Grit: %d of %d build requirements are not satisfied; compiled exports will not work until they are.", missing, items.size()));
	}
}
