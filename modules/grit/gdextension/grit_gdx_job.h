#pragma once

#include "grit_gdx_apply.h"

#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/thread.h"

#include <atomic>

// Runs the slow part of an extension action on a worker thread so the editor
// keeps painting behind the progress dialog.
//
//   add:    prepare() on the main thread, build() + finish() staged
//   revert: the whole revert runs on the worker (file IO plus build)
class GritGdxJob : public RefCounted {
public:
	~GritGdxJob();

	// Main thread. Adding a class: generation happens here, the build in run().
	Error start(const String &p_script_path, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path);
	// Main thread. Reverting a class: everything happens on the worker.
	Error start_revert(const String &p_class_name, const String &p_extension_directory, const String &p_scene_path, const String &p_node_path);

	bool is_finished() const { return finished.load(); }
	int get_progress() const { return progress.load(); }
	String get_status() const;
	String get_extension_directory() const;
	String get_report() const;

	// Main thread, once is_finished(): joins and returns the action result.
	Error complete(String &r_report);

private:
	enum class Mode {
		ADD,
		REVERT,
	};

	static void _thread(void *p_userdata);
	void run();

	Mode mode = Mode::ADD;
	Thread thread;
	bool joined = false;
	std::atomic<bool> finished{ false };
	std::atomic<int> progress{ 0 };
	mutable Mutex mutex;
	String status;
	String report;
	Error prepare_error = OK;
	Error build_error = OK;
	GritGdxClass klass;
	GritGdxApply::Result result;
	String revert_class;
};
