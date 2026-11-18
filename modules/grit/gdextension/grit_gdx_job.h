#pragma once

#include "grit_gdx_apply.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"

#include <atomic>

// Runs the GDExtension library build on a worker thread so the editor keeps
// painting behind the progress dialog. Generation happens on the main thread.
class GritGdxJob : public RefCounted {
public:
	~GritGdxJob();

	// Main thread. Regenerates the project extension's classes from the `.gde`
	// sources (no scene is touched) and starts the build on the worker.
	Error start_build_source(const String &p_script_path);

	bool is_finished() const { return finished.load(); }
	int get_progress() const { return progress.load(); }
	String get_status() const;

	// Main thread, once is_finished(): joins and returns the action result.
	Error complete(String &r_report);

private:
	static void _thread(void *p_userdata);
	void run();

	Thread thread;
	bool joined = false;
	std::atomic<bool> finished{ false };
	std::atomic<int> progress{ 0 };
	mutable Mutex mutex;
	String status;
	String report;
	Error prepare_error = OK;
	Error build_error = OK;
	GritGdxApply::Result result;
};
