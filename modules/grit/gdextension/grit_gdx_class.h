#pragma once

#include "grit/compiler/grit_ir.h"

#include "modules/gdscript/gdscript_function.h"

#include "core/object/object.h"
#include "core/string/ustring.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/variant.h"

// Metadata for one GDScript variable that becomes a field of the generated
// native class.
struct GritGdxProperty {
	String name; // GDScript name, also the registered property name.
	String field; // C++ field, "member_<name>".
	String getter; // "get_<name>"
	String setter; // "set_<name>"
	PropertyInfo info; // type/hint/usage as the script declared it.
	GDScriptDataType type;
	Variant::Type storage = Variant::NIL; // NIL means the field is a Variant.
	Variant default_value;
	bool is_export = false;
	bool has_default = false;
};

struct GritGdxSignal {
	StringName name;
	LocalVector<PropertyInfo> arguments;
};

struct GritGdxSkipped {
	String name;
	String reason;
};

// Everything the emitter needs to print one native class.
struct GritGdxClass {
	String script_path;
	String class_name;
	StringName base_type;
	bool is_tool = false;
	LocalVector<GritGdxProperty> properties;
	LocalVector<GritGdxSignal> signals;
	LocalVector<GritFunction> functions;
	LocalVector<GritGdxSkipped> skipped;
	// GDScript member index (Address::MEMBER) -> property index.
	HashMap<int, int> property_by_member_index;
};
