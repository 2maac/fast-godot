#include "grit_gdx_job.h"

#include "core/string/string_builder.h"

GritGdxJob::~GritGdxJob() {
	if (thread.is_started() && !joined) {
		thread.wait_to_finish();
	}
}

Error GritGdxJob::start_build_source(const String &p_script_path) {
	result = GritGdxApply::Result();
	result.script_path = p_script_path;
	{
		MutexLock lock(mutex);
		status = "Generating the project classes";
	}
	progress.store(5);
	String generate_report;
	prepare_error = GritGdxApply::refresh_project_extension(generate_report);
	result.extension_directory = GritGdxApply::project_extension_directory();
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

	String build_output;
	String library;
	const Error build_status = GritGdxApply::build(result.extension_directory, "editor", progress_callback, library, build_output);
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
	finished.store(true);
}

String GritGdxJob::get_status() const {
	MutexLock lock(mutex);
	return status;
}

Error GritGdxJob::complete(String &r_report) {
	if (thread.is_started() && !joined) {
		thread.wait_to_finish();
		joined = true;
	}
	if (prepare_error != OK) {
		MutexLock lock(mutex);
		r_report = report;
		return prepare_error;
	}
	if (build_error != OK) {
		MutexLock lock(mutex);
		r_report = report;
		return build_error;
	}
	{
		MutexLock lock(mutex);
		r_report = report;
		status = "Done";
	}
	progress.store(100);
	return OK;
}
