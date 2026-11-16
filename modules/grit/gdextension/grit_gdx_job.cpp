#include "grit_gdx_job.h"

#include "grit_gdx_builder.h"

#include "core/string/string_builder.h"

GritGdxJob::~GritGdxJob() {
	if (thread.is_started() && !joined) {
		thread.wait_to_finish();
	}
}

Error GritGdxJob::start(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path) {
	mode = Mode::ADD;
	GritGdxBuilder::resolve_paths();

	String prepare_report;
	prepare_error = GritGdxApply::prepare(p_script_path, p_extension_directory, p_scene_path, p_node_path, klass, result, prepare_report);
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
	if (mode == Mode::ADD) {
		String build_output;
		String library;
		const Error build_status = GritGdxApply::build(result.extension_directory, "template_debug", progress_callback, library, build_output);
		if (build_status == OK) {
			result.built = true;
			result.library_path = library;
		} else {
			build_error = build_status;
			MutexLock lock(mutex);
			report = report + "\nGrit: the build failed:\n" + build_output;
			status = "The build failed";
		}
	} else {
		const Error revert_status = GritGdxApply::revert(revert_class, result.extension_directory, result.scene_path, result.node_path, true, progress_callback, result, action_report);
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
