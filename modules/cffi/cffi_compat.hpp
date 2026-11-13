#pragma once

#include "core/object/class_db.h"
#include "core/string/ustring.h"
#include "core/templates/span.h"
#include "core/variant/array.h"
#include "core/variant/variant.h"

// godot-cpp provides Array::make(...); the engine does not. Fold expressions
// keep the call sites unchanged.
template <typename... Args>
static inline Array make_array(Args &&...p_args) {
	Array array;
	(array.push_back(Variant(p_args)), ...);
	return array;
}

// godot-cpp also provides String::operator%, which the engine's String does not
// have. Mapping it onto String::sprintf keeps the format strings untouched.
inline String operator%(const String &p_format, const Variant &p_value) {
	bool error = false;
	if (p_value.get_type() == Variant::ARRAY) {
		Array array = p_value;
		return p_format.sprintf(array.span(), &error);
	}
	Variant value = p_value;
	return p_format.sprintf(Span<Variant>(&value, 1), &error);
}
