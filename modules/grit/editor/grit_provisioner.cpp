#include "grit_provisioner.h"

#include "core/config/project_settings.h"
#include "core/crypto/hashing_context.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "core/version.h"
#include "editor/editor_node.h"
#include "editor/settings/editor_settings.h"
#include "servers/display/display_server.h"

// The toolchain folder is per machine and self-contained: its toolchain.json lists
// the components, including the engine source tree the templates build from. A
// checkout next to the editor is used instead only for a staged/dev build, which
// must build against its own tree.

void GritProvisioner::register_settings() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings) {
		return;
	}
	// The toolchain folder is per machine, starts empty, and is only filled in by
	// the user or by Download Build Tools.
	if (settings->has_setting("grit/settings/toolchain_root")) {
		const String old = String(settings->get_setting("grit/settings/toolchain_root")).strip_edges();
		if (!old.is_empty()) {
			settings->set_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT, old);
		}
		settings->erase("grit/settings/toolchain_root");
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
}

void GritProvisioner::purge_obsolete_settings() {
	EditorSettings *editor = EditorSettings::get_singleton();
	if (editor) {
		List<PropertyInfo> props;
		editor->get_property_list(&props);
		Vector<String> stale;
		for (const PropertyInfo &prop : props) {
			const String name = prop.name;
			// Anything grit-scoped that is not the toolchain setting we keep.
			if (name.begins_with("grit/") && name != GritProvisioner::SETTING_TOOLCHAIN_ROOT) {
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

static String _toolchain_root() {
	EditorSettings *settings = EditorSettings::get_singleton();
	if (!settings || !settings->has_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT)) {
		return String();
	}
	return String(settings->get_setting(GritProvisioner::SETTING_TOOLCHAIN_ROOT)).strip_edges();
}

static bool _toolchain_usable(const String &p_root) {
	return !p_root.is_empty() && FileAccess::exists(p_root.path_join("toolchain.json"));
}

// The commit this editor was built from; the one-time toolchain bootstrap fetches
// exactly this revision so the downloader runs against a matching tree.
String GritProvisioner::engine_commit() {
	const String hash = String(VERSION_HASH);
	if (hash.is_empty() || hash == "unknown") {
		return String();
	}
	return hash;
}

// GitHub generates this archive for any commit, so the source needs no release
// asset of its own. It also pins the fetch to the exact tree this editor was built
// from: the release tag is moved on every publish, so a tag archive would hand an
// older editor a different engine revision.
static String _engine_archive_url(const String &p_commit) {
	return vformat("https://github.com/2maac/fast-godot/archive/%s.zip", p_commit);
}

static bool _engine_present(const String &p_dir) {
	return FileAccess::exists(p_dir.path_join("SConstruct")) &&
			FileAccess::exists(p_dir.path_join("tools").path_join("grit_build.py"));
}

// DirAccess::remove_absolute() removes a file or an empty directory; a populated
// tree (the unpacked engine source is thousands of files) needs its contents
// erased first.
static void _remove_tree(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_valid()) {
		dir->erase_contents_recursive();
	}
	DirAccess::remove_absolute(p_path);
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
	EditorProgress progress("grit_engine_download", TTR("Downloading the Fast_Godot toolchain"), 100, true);

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

static bool _toolchain_complete(const String &p_root) {
	if (p_root.is_empty() || !FileAccess::exists(p_root.path_join("toolchain.json"))) {
		return false;
	}
	if (!DirAccess::dir_exists_absolute(p_root.path_join("python"))) {
		return false;
	}
	if (!FileAccess::exists(p_root.path_join("scons").path_join("4.11.1").path_join("scons-4.11.1").path_join("SCons").path_join("__init__.py"))) {
		return false;
	}
	if (!DirAccess::dir_exists_absolute(p_root.path_join("engine"))) {
		return false;
	}
	const String os = OS::get_singleton()->get_name();
	if (os == "Windows") {
		return FileAccess::exists(p_root.path_join("mingw64").path_join("bin").path_join("gcc.exe")) &&
				FileAccess::exists(p_root.path_join("libffi").path_join("3.8.0").path_join("mingw64").path_join("lib").path_join("libffi.a")) &&
				DirAccess::dir_exists_absolute(p_root.path_join("angle")) &&
				DirAccess::dir_exists_absolute(p_root.path_join("mesa-x86_64-gcc")) &&
				DirAccess::dir_exists_absolute(p_root.path_join("agility_sdk")) &&
				DirAccess::dir_exists_absolute(p_root.path_join("winrt"));
	}
	if (os == "macOS") {
		return DirAccess::dir_exists_absolute(p_root.path_join("moltenvk").path_join("macOS").path_join("lib").path_join("MoltenVK.xcframework"));
	}
	return FileAccess::exists(p_root.path_join("gcc").path_join("bin").path_join("g++")) &&
			FileAccess::exists(p_root.path_join("gcc").path_join("bin").path_join("wayland-scanner")) &&
			FileAccess::exists(p_root.path_join("libffi").path_join("linux-x86_64").path_join("include").path_join("ffi.h"));
}

bool GritProvisioner::toolchain_ready() {
	return _toolchain_complete(configured_toolchain());
}

String GritProvisioner::home_toolchain_folder() {
	String home = OS::get_singleton()->get_environment("HOME");
	if (home.is_empty()) {
		home = OS::get_singleton()->get_environment("USERPROFILE");
	}
	if (home.is_empty()) {
		home = OS::get_singleton()->get_data_path();
	}
	return home.path_join("fast_godot_toolchain");
}

bool GritProvisioner::download_toolchain(const String &p_dest, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	print_line("Grit: downloading the toolchain into " + p_dest + ".");
	const String engine = ensure_engine_for_toolchain(p_dest, &error);
	if (engine.is_empty()) {
		return false;
	}
	// The only scratch is <toolchain>/.download: it holds the progress file while
	// the downloader runs and is removed on every exit path, so a finished
	// toolchain folder contains nothing but the toolchain.
	const String scratch = p_dest.path_join(".download");
	DirAccess::make_dir_recursive_absolute(scratch);
	const String progress_path = scratch.path_join("progress.txt");
	DirAccess::remove_absolute(progress_path);
	auto cleanup_scratch = [&]() {
		_remove_tree(scratch);
	};
	const bool windows = OS::get_singleton()->get_name() == "Windows";
	const String script = engine.path_join("tools").path_join("toolchain").path_join(windows ? "download_toolchain.cmd" : "download_toolchain.sh");
	if (!FileAccess::exists(script)) {
		error = vformat("the toolchain downloader is missing from \"%s\".", engine);
		cleanup_scratch();
		return false;
	}
	if (DirAccess::make_dir_recursive_absolute(p_dest) != OK) {
		error = vformat("could not create \"%s\".", p_dest);
		cleanup_scratch();
		return false;
	}
	List<String> args;
	ProcessID pid = 0;
	String program;
	if (windows) {
		program = "cmd.exe";
		args.push_back("/c");
		args.push_back(script);
	} else {
		// The engine source can come from a GitHub archive, which does not keep the
		// executable bit; run the script through the shell instead of exec'ing it.
		program = "/bin/sh";
		args.push_back(script);
	}
	args.push_back(p_dest);
	args.push_back(progress_path);
	if (OS::get_singleton()->create_process(program, args, &pid) != OK) {
		error = "could not start the toolchain downloader.";
		cleanup_scratch();
		return false;
	}

	EditorProgress progress("grit_toolchain_download", TTR("Downloading the Fast_Godot toolchain"), 100, true);
	String state = TTR("Downloading the Fast_Godot toolchain");
	int percent = -1;
	bool finished = false;
	while (true) {
		OS::get_singleton()->delay_usec(200000);
		Ref<FileAccess> file = FileAccess::open(progress_path, FileAccess::READ);
		if (file.is_valid()) {
			const PackedStringArray lines = file->get_as_text().split("\n");
			for (int i = lines.size() - 1; i >= 0; i--) {
				const String line = lines[i].strip_edges();
				if (line.begins_with("GRIT_DONE ")) {
					finished = true;
					break;
				}
				if (line.begins_with("GRIT_ERROR ")) {
					error = line.substr(String("GRIT_ERROR ").length()).strip_edges();
					break;
				}
				if (line.begins_with("GRIT_PROGRESS ")) {
					const PackedStringArray parts = line.split(" ", false, 2);
					if (parts.size() >= 3) {
						percent = parts[1].to_int();
						state = parts[2];
					}
					break;
				}
			}
		}
		if (progress.step(state, percent)) {
			OS::get_singleton()->kill(pid);
			error = "the download was cancelled";
			cleanup_scratch();
			return false;
		}
		if (DisplayServer::get_singleton()) {
			DisplayServer::get_singleton()->process_events();
		}
		if (!OS::get_singleton()->is_process_running(pid)) {
			break;
		}
	}
	if (!error.is_empty() || !finished || !_toolchain_complete(p_dest)) {
		if (error.is_empty()) {
			error = "the downloader finished but the toolchain is incomplete.";
		}
		cleanup_scratch();
		return false;
	}
	cleanup_scratch();
	EditorSettings *settings = EditorSettings::get_singleton();
	settings->set_setting(SETTING_TOOLCHAIN_ROOT, p_dest);
	settings->save();
	return true;
}

String GritProvisioner::configured_toolchain() {
	const String root = _toolchain_root();
	if (_toolchain_usable(root)) {
		return root;
	}
	if (!root.is_empty()) {
		// An explicit setting that points nowhere: report it, do not guess.
		return String();
	}
	// The Download button installs into home_toolchain_folder(); adopt it when it
	// is already complete so the editor does not offer a download it has done.
	const String home = home_toolchain_folder();
	if (_toolchain_complete(home)) {
		EditorSettings *settings = EditorSettings::get_singleton();
		if (settings) {
			settings->set(GritProvisioner::SETTING_TOOLCHAIN_ROOT, home);
		}
		print_line(vformat("Grit: using the downloaded toolchain in \"%s\".", home));
		return home;
	}
	return String();
}

String GritProvisioner::toolchain_engine() {
	// The toolchain declares the one engine tree it carries. Do not go looking for
	// other trees under engine/: a second copy could only be a leftover.
	const String root = configured_toolchain();
	if (root.is_empty()) {
		return String();
	}
	Ref<FileAccess> file = FileAccess::open(root.path_join("toolchain.json"), FileAccess::READ);
	if (file.is_null()) {
		return String();
	}
	const Variant parsed = JSON::parse_string(file->get_as_text());
	file->close();
	if (parsed.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary data = parsed;
	if (!data.has("engine")) {
		return String();
	}
	const String relative = String(data["engine"]).strip_edges();
	if (relative.is_empty()) {
		return String();
	}
	// The toolchain is not tied to an editor revision: an engine tree from any
	// 4.7.x toolchain is used as it is, with no revision checks.
	const String tree = relative.is_relative_path() ? root.path_join(relative) : relative;
	return _engine_present(tree) ? tree : String();
}

String GritProvisioner::ensure_engine(String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	// The toolchain's downloaded engine source is the only tree used. There is no
	// fallback to a checkout or to a download: a toolchain without source is an
	// error, and the archive is only fetched while a toolchain is being created.
	const String toolchain = configured_toolchain();
	if (toolchain.is_empty()) {
		error = "no Fast_Godot build tools are installed. Open the export window and press Download Build Tools; the toolchain carries the engine source, so an export never downloads anything.";
		return String();
	}
	const String in_toolchain = toolchain_engine();
	if (!in_toolchain.is_empty()) {
		print_line("Grit: using the engine source in the toolchain at " + in_toolchain);
		return in_toolchain;
	}
	error = vformat("the toolchain at \"%s\" has no engine source: toolchain.json does not declare an \"engine\" tree, or it is missing. Use a toolchain built by tools/toolchain/download_toolchain.{sh,cmd}, which copy the engine source in.", toolchain);
	return String();
}

String GritProvisioner::ensure_engine_for_toolchain(const String &p_dest, String *r_error) {
	String local_error;
	String &error = r_error ? *r_error : local_error;

	// The engine source is unpacked straight into its final place in the toolchain;
	// nothing is copied afterwards. The only scratch is <toolchain>/.download,
	// which holds the archive while it is unpacked and is deleted right after.
	// The folder has no revision in its name: the toolchain works with any 4.7.x
	// editor, so there is nothing to match.
	const String engine_dir = p_dest.path_join("engine").path_join("source");
	if (_engine_present(engine_dir)) {
		// A previous attempt already unpacked the source; continue from there.
		return engine_dir;
	}

	const String scratch = p_dest.path_join(".download");
	_remove_tree(scratch);
	DirAccess::make_dir_recursive_absolute(scratch);
	const String archive = scratch.path_join("engine.zip");
	const String unpacked = scratch.path_join("unpacked");
	DirAccess::make_dir_recursive_absolute(unpacked);

	// Prefer this editor's revision; a build without a revision uses the main
	// branch. Either way the source is only needed to create the toolchain, and
	// the finished toolchain is not tied to it.
	const String commit = engine_commit();
	const String url = commit.is_empty()
			? String("https://github.com/2maac/fast-godot/archive/refs/heads/fresh.zip")
			: _engine_archive_url(commit);
	String download_error;
	if (!_download_engine_archive(url, archive, download_error)) {
		_remove_tree(scratch);
		error = vformat("could not download the engine source from %s (%s).", url, download_error);
		return String();
	}

	// Unpack with Python when it works (it is portable), otherwise with tar, which
	// reads zip on Windows and macOS. One of the two always applies.
	bool unpacked_ok = false;
	String unpack_error;
	const String python = python_for_build();
	if (!python.is_empty()) {
		List<String> args;
		args.push_back("-m");
		args.push_back("zipfile");
		args.push_back("-e");
		args.push_back(archive);
		args.push_back(unpacked);
		int code = -1;
		String output;
		const Error ran = OS::get_singleton()->execute(python, args, &output, &code);
		unpacked_ok = ran == OK && code == 0;
		if (!unpacked_ok) {
			unpack_error = vformat("python %s exited %d: %s", python, code, output.strip_edges().right(160));
		}
	}
	if (!unpacked_ok) {
		String tar_error;
		if (_extract(archive, unpacked, tar_error)) {
			unpacked_ok = true;
		} else {
			unpack_error += (unpack_error.is_empty() ? "" : "; ") + tar_error;
		}
	}
	DirAccess::remove_absolute(archive);
	if (!unpacked_ok) {
		_remove_tree(scratch);
		error = vformat("could not unpack the engine source downloaded from %s (%s).", url, unpack_error);
		return String();
	}

	// GitHub archives wrap the tree in fast-godot-<revision>/; move that folder to
	// its final place. A rename keeps the source where it was unpacked, so the
	// engine source is only ever written once.
	String tree = unpacked;
	Ref<DirAccess> dir = DirAccess::open(unpacked);
	if (dir.is_valid()) {
		dir->list_dir_begin();
		for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
			if (dir->current_is_dir() && FileAccess::exists(unpacked.path_join(name).path_join("SConstruct"))) {
				tree = unpacked.path_join(name);
				break;
			}
		}
		dir->list_dir_end();
	}
	if (!_engine_present(tree)) {
		_remove_tree(scratch);
		error = vformat("the engine source downloaded from %s is not a usable Fast_Godot tree.", url);
		return String();
	}
	DirAccess::make_dir_recursive_absolute(p_dest.path_join("engine"));
	_remove_tree(engine_dir);
	const Error moved = DirAccess::rename_absolute(tree, engine_dir);
	_remove_tree(scratch);
	if (moved != OK || !_engine_present(engine_dir)) {
		error = vformat("could not move the engine source into \"%s\".", engine_dir);
		return String();
	}
	return engine_dir;
}

String GritProvisioner::python_for_build() {
	const String root = _toolchain_root();
	if (_toolchain_usable(root)) {
		String bundled = root.path_join("python").path_join(
				OS::get_singleton()->get_name() == "Windows" ? "python.exe" : "bin/python3");
		// Windows will not reliably start a program whose path uses forward
		// slashes, and this value is handed straight to create_process.
		if (OS::get_singleton()->get_name() == "Windows") {
			bundled = bundled.replace("/", "\\");
		}
		if (FileAccess::exists(bundled)) {
			// A toolchain unpacked by an editor that used python's zipfile lost
			// the executable bits, and running it then fails with exit 126. Do
			// not hand that to the build: fall back to the system interpreter.
			List<String> probe;
			probe.push_back("--version");
			String output;
			int code = -1;
			OS::get_singleton()->execute(bundled, probe, &output, &code);
			if (code == 0) {
				return bundled;
			}
			WARN_PRINT(vformat("Grit: the toolchain python at \"%s\" cannot run (exit %d); using the system interpreter instead. If the build fails, re-download the toolchain.", bundled, code));
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
	json += "  \"toolchain_root\": \"" + root.replace("\\", "/") + "\",\n";
	json += "  \"path_add\": [";
	for (int i = 0; i < path_add.size(); i++) {
		json += String(i == 0 ? "" : ", ") + "\"" + path_add[i].replace("\\", "/") + "\"";
	}
	json += "]\n}\n";
	file->store_string(json);
	file->close();
}

void GritProvisioner::run_download_probe() {
	static bool ran = false;
	if (ran) {
		return;
	}
	ran = true;
	String error;

	if (download_toolchain(home_toolchain_folder(), &error)) {
		print_line("Grit: download probe: toolchain ready at " + home_toolchain_folder() + ".");
	} else {
		print_line("Grit: download probe failed: " + error);
	}
}

void GritProvisioner::run_probe() {
	static bool ran = false;
	if (ran) {
		return;
	}
	ran = true;

	String error;
	if (!ensure_engine(&error).is_empty()) {
		print_line("Grit: build tools ready.");
	} else {
		ERR_PRINT("Grit: " + error);
	}
}
