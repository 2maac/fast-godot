#pragma once

#include "modules/gdscript/gdscript.h"

#include "core/io/resource_loader.h"
#include "core/io/resource_saver.h"

// The archived source of a converted script: a GDScript that is never compiled
// and never instantiated. `can_instantiate()` returning false makes
// `Object::set_script` create a placeholder in the editor and nothing at runtime,
// so the node keeps showing its script while only the generated GDExtension class
// runs. The archive stays at the script's path with a `.gde` extension and can be
// edited in the script editor; Convert Back turns it into a normal `.gd` again.
class GritGdxScript : public GDScript {
	GDCLASS(GritGdxScript, GDScript);

	StringName archived_base_type;
	String archived_class;

protected:
	static void _bind_methods() {}

public:
	void set_archived_metadata(const StringName &p_base_type, const String &p_class);
	StringName get_archived_base_type() const { return archived_base_type; }
	String get_archived_class() const { return archived_class; }

	// Never instantiate: the generated class is the implementation, the archive is
	// only its readable source.
	bool can_instantiate() const override { return false; }
#ifdef TOOLS_ENABLED
	// The archived script shows the red GDScript icon in the scene tree.
	StringName get_editor_icon() const override;
#endif
	Error reload(bool p_keep_state = false) override { return OK; }
	StringName get_instance_base_type() const override { return archived_base_type; }
};

// `.gde` files are plain GDScript text. Loading one builds an inert GritGdxScript
// instead of compiling a GDScript.
class GritGdxScriptLoader : public ResourceFormatLoader {
	GDSOFTCLASS(GritGdxScriptLoader, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};

class GritGdxScriptSaver : public ResourceFormatSaver {
	GDSOFTCLASS(GritGdxScriptSaver, ResourceFormatSaver);

public:
	virtual Error save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags = 0) override;
	virtual void get_recognized_extensions(const Ref<Resource> &p_resource, List<String> *p_extensions) const override;
	virtual bool recognize(const Ref<Resource> &p_resource) const override;
};
