#include "grit_gdx_job.h"

#include "grit_gdx_builder.h"

#include "core/string/string_builder.h"

GritGdxJob::~GritGdxJob() {
	if (thread.is_started() && !joined) {
		thread.wait_to_finish();
	}
}

Error GritGdxJob::start(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path, bool p_allow_partial) {
	mode = Mode::ADD;
	GritGdxBuilder::resolve_paths();

	String prepare_report;
	prepare_error = GritGdxApply::prepare(p_script_path, p_extension_directory, p_scene_path, p_node_path, p_allow_partial, klass, result, prepare_report);
	report = prepare_report;
	{
		MutexLock lock(mutex);
		status = prepare_error == OK ? "Generating the native class" : "Generation failed";
	}
	if (prepare_error != OK) {
		finished.store(true);
		return prepare_error;
	}
	progress.store(5);
	thread.start(_thread, this);
	return OK;
}

Error GritGdxJob::start_revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path) {
	mode = Mode::REVERT;
	revert_class = p_class_name;
	result = GritGdxApply::Result();
	result.class_name = p_class_name;
	result.extension_directory = p_extension_directory;
	result.scene_path = p_scene_path;
	result.node_path = p_node_path;
	{
		MutexLock lock(mutex);
		status = "Reverting to GDScript";
	}
	progress.store(20);
	thread.start(_thread, this);
	return OK;
}

Error GritGdxJob::start_rebuild(const String &p_class_name, const String &p_extension_directory) {
	mode = Mode::REBUILD;
	{
		MutexLock lock(mutex);
		status = "Regenerating the native class";
	}
	progress.store(5);
	prepare_error = GritGdxApply::begin_rebuild(p_class_name, p_extension_directory, rebuild_state);
	report = rebuild_state.report;
	result = rebuild_state.result;
	if (prepare_error != OK) {
		finished.store(true);
		return prepare_error;
	}
	thread.start(_thread, this);
	return OK;
}

Error GritGdxJob::start_build_source(const String &p_script_path) {
	mode = Mode::BUILD_SOURCE;
	result = GritGdxApply::Result();
	result.script_path = p_script_path;
	{
		MutexLock lock(mutex);
		status = "Generating the native class";
	}
	progress.store(5);
	String generate_report;
	String directory;
	prepare_error = GritGdxApply::create_source(p_script_path, generate_report, &directory);
	result.extension_directory = directory;
	{
		MutexLock lock(mutex);
		report = generate_report;
	}
	if (prepare_error != OK) {
		finished.store(true);
		return prepare_error;
	}
	progress.store(20);
	thread.start(_thread, this);
	return OK;
}

void GritGdxJob::_thread(void *p_userdata) {
	static_cast<GritGdxJob *>(p_userdata)->run();
}

void GritGdxJob::run() {
	const GritGdxApply::Progress progress_callback = [this](int p_progress, const String &p_status) {
		progress.store(p_progress);
		MutexLock lock(mutex);
		status = p_status;
	};

	String action_report;
	if (mode == Mode::ADD || mode == Mode::REBUILD || mode == Mode::BUILD_SOURCE) {
		const String target = mode == Mode::BUILD_SOURCE ? "editor" : "template_debug";
		String build_output;
		String library;
		const Error build_status = GritGdxApply::build(result.extension_directory, target, progress_callback, library, build_output);
		if (build_status == OK) {
			result.built = true;
			result.library_path = library;
			MutexLock lock(mutex);
			report = report + vformat("\n  library: %s\n", library);
		} else {
			build_error = build_status;
			MutexLock lock(mutex);
			report = report + "\nGrit: the build failed:\n" + build_output;
			status = "The build failed";
		}
	} else {
		// `revert` resets `result` immediately, so the arguments must not alias it.
		const String extension_directory = result.extension_directory;
		const String scene_path = result.scene_path;
		const String node_path = result.node_path;
		const Error revert_status = GritGdxApply::revert(revert_class, extension_directory, scene_path, node_path, true, progress_callback, result, action_report);
		build_error = revert_status;
		MutexLock lock(mutex);
		report = action_report;
		status = revert_status == OK ? "Done" : "Revert failed";
	}
	finished.store(true);
}

String GritGdxJob::get_status() const {
	MutexLock lock(mutex);
	return status;
}

String GritGdxJob::get_extension_directory() const {
	return result.extension_directory;
}

String GritGdxJob::get_report() const {
	MutexLock lock(mutex);
	return report;
}

Error GritGdxJob::complete(String &r_report) {
	if (thread.is_started() && !joined) {
		thread.wait_to_finish();
		joined = true;
	}
	if (mode == Mode::REVERT) {
		r_report = get_report();
		return build_error;
	}
	if (prepare_error != OK) {
		r_report = get_report();
		return prepare_error;
	}
	if (build_error != OK) {
		r_report = get_report();
		return build_error;
	}

	if (mode == Mode::REBUILD) {
		{
			MutexLock lock(mutex);
			status = "Recording the new source";
		}
		progress.store(95);
		rebuild_state.result = result;
		String finish_report = get_report();
		const Error finish_status = GritGdxApply::finish_rebuild(rebuild_state, true, finish_report);
		r_report = finish_report;
		if (finish_status == OK) {
			progress.store(100);
		}
		{
			MutexLock lock(mutex);
			status = finish_status == OK ? "Done" : "Refused";
		}
		return finish_status;
	}

	{
		MutexLock lock(mutex);
		status = "Rewriting the node";
	}
	progress.store(95);
	String finish_report = get_report();
	const Error finish_status = GritGdxApply::finish(klass, true, result, finish_report);
	r_report = finish_report;
	if (finish_status == OK) {
		progress.store(100);
	}
	{
		MutexLock lock(mutex);
		status = finish_status == OK ? "Done" : "Refused";
	}
	return finish_status;
}
