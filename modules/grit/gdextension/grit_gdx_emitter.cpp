#include "grit_gdx_emitter.h"

#include "core/math/math_funcs.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/string/string_builder.h"

namespace {

struct VirtualParam {
	const char *type;
	const char *name;
};

struct VirtualInfo {
	const char *name;
	const char *return_type;
	const VirtualParam *params;
	int param_count;
};

const VirtualParam NO_PARAMS[] = { { nullptr, nullptr } };
const VirtualParam DELTA_PARAMS[] = { { "double", "p_delta" } };
const VirtualParam EVENT_PARAMS[] = { { "const Ref<InputEvent> &", "p_event" } };
const VirtualParam NOTIFICATION_PARAMS[] = { { "int32_t", "p_what" } };

const VirtualInfo VIRTUALS[] = {
	{ "_ready", "void", NO_PARAMS, 0 },
	{ "_process", "void", DELTA_PARAMS, 1 },
	{ "_physics_process", "void", DELTA_PARAMS, 1 },
	{ "_enter_tree", "void", NO_PARAMS, 0 },
	{ "_exit_tree", "void", NO_PARAMS, 0 },
	{ "_input", "void", EVENT_PARAMS, 1 },
	{ "_shortcut_input", "void", EVENT_PARAMS, 1 },
	{ "_unhandled_input", "void", EVENT_PARAMS, 1 },
	{ "_unhandled_key_input", "void", EVENT_PARAMS, 1 },
	{ "_gui_input", "void", EVENT_PARAMS, 1 },
	{ "_draw", "void", NO_PARAMS, 0 },
	{ "_notification", "void", NOTIFICATION_PARAMS, 1 },
};

const VirtualInfo *find_virtual(const String &p_name) {
	for (const VirtualInfo &info : VIRTUALS) {
		if (p_name == String(info.name)) {
			return &info;
		}
	}
	return nullptr;
}

const char *operator_code(Variant::Operator p_operator) {
	switch (p_operator) {
		case Variant::OP_ADD:
			return "Variant::OP_ADD";
		case Variant::OP_SUBTRACT:
			return "Variant::OP_SUBTRACT";
		case Variant::OP_MULTIPLY:
			return "Variant::OP_MULTIPLY";
		case Variant::OP_DIVIDE:
			return "Variant::OP_DIVIDE";
		case Variant::OP_MODULE:
			return "Variant::OP_MODULE";
		case Variant::OP_EQUAL:
			return "Variant::OP_EQUAL";
		case Variant::OP_NOT_EQUAL:
			return "Variant::OP_NOT_EQUAL";
		case Variant::OP_LESS:
			return "Variant::OP_LESS";
		case Variant::OP_LESS_EQUAL:
			return "Variant::OP_LESS_EQUAL";
		case Variant::OP_GREATER:
			return "Variant::OP_GREATER";
		case Variant::OP_GREATER_EQUAL:
			return "Variant::OP_GREATER_EQUAL";
		default:
			return nullptr;
	}
}

String variant_enum(const Variant::Type p_type) {
	switch (p_type) {
		case Variant::NIL:
			return "NIL";
		case Variant::BOOL:
			return "BOOL";
		case Variant::INT:
			return "INT";
		case Variant::FLOAT:
			return "FLOAT";
		case Variant::STRING:
			return "STRING";
		case Variant::STRING_NAME:
			return "STRING_NAME";
		case Variant::NODE_PATH:
			return "NODE_PATH";
		case Variant::RID:
			return "RID";
		case Variant::VECTOR2:
			return "VECTOR2";
		case Variant::VECTOR2I:
			return "VECTOR2I";
		case Variant::RECT2:
			return "RECT2";
		case Variant::RECT2I:
			return "RECT2I";
		case Variant::VECTOR3:
			return "VECTOR3";
		case Variant::VECTOR3I:
			return "VECTOR3I";
		case Variant::TRANSFORM2D:
			return "TRANSFORM2D";
		case Variant::VECTOR4:
			return "VECTOR4";
		case Variant::VECTOR4I:
			return "VECTOR4I";
		case Variant::PLANE:
			return "PLANE";
		case Variant::QUATERNION:
			return "QUATERNION";
		case Variant::AABB:
			return "AABB";
		case Variant::BASIS:
			return "BASIS";
		case Variant::TRANSFORM3D:
			return "TRANSFORM3D";
		case Variant::PROJECTION:
			return "PROJECTION";
		case Variant::COLOR:
			return "COLOR";
		case Variant::ARRAY:
			return "ARRAY";
		case Variant::DICTIONARY:
			return "DICTIONARY";
		case Variant::OBJECT:
			return "OBJECT";
		case Variant::CALLABLE:
			return "CALLABLE";
		case Variant::SIGNAL:
			return "SIGNAL";
		default:
			return "NIL";
	}
}

bool is_gdscript_utility(const String &p_name, const String &p_arguments, String &r_expression) {
	if (p_name == "int") {
		r_expression = "static_cast<int64_t>(" + p_arguments + ")";
		return true;
	}
	if (p_name == "float") {
		r_expression = "static_cast<double>(" + p_arguments + ")";
		return true;
	}
	if (p_name == "str" || p_name == "bool") {
		r_expression = "UtilityFunctions::" + p_name + "(" + p_arguments + ")";
		return true;
	}
	return false;
}

} // namespace

GritGdxEmitter::GritGdxEmitter(const GritGdxClass &p_class) :
		klass(p_class) {
	for (const GritFunction &function_entry : klass.functions) {
		methods.insert(function_entry.name, &function_entry);
	}
	for (const GritGdxProperty &property : klass.properties) {
		properties_by_name.insert(property.name, &property);
	}
}

void GritGdxEmitter::line(const String &p_text) {
	for (int i = 0; i < indentation; i++) {
		body.append("\t");
	}
	body.append(p_text);
	body.append("\n");
}

void GritGdxEmitter::reject(const String &p_reason) {
	if (reject_reason.is_empty()) {
		reject_reason = p_reason;
	}
}

bool GritGdxEmitter::is_supported(const String &p_name) const {
	const bool *supported = supported_methods.getptr(p_name);
	return supported && *supported;
}

void GritGdxEmitter::prepare() {
	supported_methods.clear();
	await_state_fields.clear();
	for (const GritFunction &function_entry : klass.functions) {
		StringBuilder probe;
		const bool is_virtual = is_virtual_name(String(function_entry.name));
		supported_methods.insert(function_entry.name, emit_function(function_entry, is_virtual, probe));
	}
	reject_reason = String();
}

String GritGdxEmitter::snake_case(const String &p_name) {
	// Mirrors godot-cpp's camel_to_snake(), which decides the generated header
	// names (node2d.hpp, a_star2d.hpp, http_request.hpp).
	String step1;
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if (i > 0 && c >= 'A' && c <= 'Z' && i + 1 < p_name.length() && p_name[i + 1] >= 'a' && p_name[i + 1] <= 'z') {
			step1 += "_";
		}
		step1 += String::chr(c);
	}
	String step2;
	for (int i = 0; i < step1.length(); i++) {
		const char32_t c = step1[i];
		if (i > 0 && c >= 'A' && c <= 'Z') {
			const char32_t previous = step1[i - 1];
			if ((previous >= 'a' && previous <= 'z') || (previous >= '0' && previous <= '9')) {
				step2 += "_";
			}
		}
		step2 += String::chr(c);
	}
	step2 = step2.replace("2_D", "2D").replace("3_D", "3D");
	return step2.to_lower();
}

String GritGdxEmitter::sanitize(const String &p_name) {
	String out;
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') {
			out += String::chr(c);
		}
	}
	return out.is_empty() ? String("value") : out;
}

String GritGdxEmitter::cpp_storage(Variant::Type p_type) {
	switch (p_type) {
		case Variant::NIL:
			return "Variant";
		case Variant::BOOL:
			return "bool";
		case Variant::INT:
			return "int64_t";
		case Variant::FLOAT:
			return "double";
		case Variant::STRING:
			return "String";
		case Variant::STRING_NAME:
			return "StringName";
		case Variant::NODE_PATH:
			return "NodePath";
		case Variant::RID:
			return "RID";
		case Variant::VECTOR2:
			return "Vector2";
		case Variant::VECTOR2I:
			return "Vector2i";
		case Variant::RECT2:
			return "Rect2";
		case Variant::RECT2I:
			return "Rect2i";
		case Variant::VECTOR3:
			return "Vector3";
		case Variant::VECTOR3I:
			return "Vector3i";
		case Variant::TRANSFORM2D:
			return "Transform2D";
		case Variant::VECTOR4:
			return "Vector4";
		case Variant::VECTOR4I:
			return "Vector4i";
		case Variant::PLANE:
			return "Plane";
		case Variant::QUATERNION:
			return "Quaternion";
		case Variant::AABB:
			return "AABB";
		case Variant::BASIS:
			return "Basis";
		case Variant::TRANSFORM3D:
			return "Transform3D";
		case Variant::PROJECTION:
			return "Projection";
		case Variant::COLOR:
			return "Color";
		case Variant::PACKED_BYTE_ARRAY:
			return "PackedByteArray";
		case Variant::PACKED_INT32_ARRAY:
			return "PackedInt32Array";
		case Variant::PACKED_INT64_ARRAY:
			return "PackedInt64Array";
		case Variant::PACKED_FLOAT32_ARRAY:
			return "PackedFloat32Array";
		case Variant::PACKED_FLOAT64_ARRAY:
			return "PackedFloat64Array";
		case Variant::PACKED_STRING_ARRAY:
			return "PackedStringArray";
		case Variant::PACKED_VECTOR2_ARRAY:
			return "PackedVector2Array";
		case Variant::PACKED_VECTOR3_ARRAY:
			return "PackedVector3Array";
		case Variant::PACKED_COLOR_ARRAY:
			return "PackedColorArray";
		case Variant::PACKED_VECTOR4_ARRAY:
			return "PackedVector4Array";
		case Variant::ARRAY:
			return "Array";
		case Variant::DICTIONARY:
			return "Dictionary";
		case Variant::CALLABLE:
			return "Callable";
		case Variant::SIGNAL:
			return "Signal";
		case Variant::OBJECT:
			return "Object *";
		default:
			return "Variant";
	}
}

String GritGdxEmitter::default_storage_value(Variant::Type p_type) {
	switch (p_type) {
		case Variant::NIL:
			return "Variant()";
		case Variant::BOOL:
			return "false";
		case Variant::INT:
			return "0";
		case Variant::FLOAT:
			return "0.0";
		case Variant::OBJECT:
			return "nullptr";
		default:
			return cpp_storage(p_type) + "()";
	}
}

String GritGdxEmitter::escaped_string(const String &p_text) {
	String out = "\"";
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		switch (c) {
			case '\\':
				out += "\\\\";
				break;
			case '"':
				out += "\\\"";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20) {
					out += vformat("\\x%02x", int(c));
				} else {
					out += String::chr(c);
				}
				break;
		}
	}
	return out + "\"";
}

String GritGdxEmitter::float_literal(double p_value) {
	String text = String::num(p_value, 17).strip_edges();
	if (!text.contains(".") && !text.contains("e") && !text.contains("E")) {
		text += ".0";
	}
	return text;
}

String GritGdxEmitter::variable_base_name(int p_variable) const {
	const GritVariable &variable = function->variables[p_variable];
	String name;
	switch (variable.role) {
		case GritVariable::Role::PARAMETER:
			name = "parameter_";
			break;
		case GritVariable::Role::LOCAL:
			name = "local_";
			break;
		case GritVariable::Role::TEMPORARY:
			name = "temporary_";
			break;
	}
	name += itos(p_variable);
	const String suffix = sanitize(variable.name);
	if (!suffix.is_empty()) {
		name += "_" + suffix;
	}
	return name;
}

String GritGdxEmitter::variable_name(int p_variable) const {
	const String base = variable_base_name(p_variable);
	return coroutine ? "p_state->" + base : base;
}

// Local names live in the coroutine state object, so every read and write goes
// through this prefix instead of a C++ stack slot.
String GritGdxEmitter::state_reference(const String &p_name) const {
	return coroutine ? "p_state->" + p_name : p_name;
}

void GritGdxEmitter::add_state_field(const String &p_type, const String &p_name, const String &p_initializer) {
	StateField field;
	field.type = p_type;
	field.name = p_name;
	field.initializer = p_initializer;
	state_fields.push_back(field);
}

String GritGdxEmitter::await_struct_name(const String &p_function) {
	return "GritAwait_" + sanitize(p_function);
}

String GritGdxEmitter::await_step_method(const String &p_function) {
	return "_grit_step_" + sanitize(p_function);
}

String GritGdxEmitter::await_resume_method(const String &p_function) {
	return "_grit_resume_" + sanitize(p_function);
}

String GritGdxEmitter::await_map_name(const String &p_function) {
	return "grit_awaits_" + sanitize(p_function);
}

const GritGdxProperty *GritGdxEmitter::property_of(int p_member) const {
	const int *index = klass.property_by_member_index.getptr(p_member);
	return index ? &klass.properties[*index] : nullptr;
}

const GritFunction *GritGdxEmitter::method_named(const String &p_name) const {
	const GritFunction *const *method = methods.getptr(p_name);
	return method ? *method : nullptr;
}

Variant::Type GritGdxEmitter::storage_of(const GritOperand &p_operand) const {
	switch (p_operand.kind) {
		case GritOperand::Kind::VARIABLE:
			return function->variables[p_operand.index].storage;
		case GritOperand::Kind::MEMBER: {
			const GritGdxProperty *property = property_of(p_operand.index);
			return property ? property->storage : Variant::NIL;
		}
		case GritOperand::Kind::CONSTANT: {
			const Variant::Type type = function->constant_types[p_operand.index];
			switch (type) {
				case Variant::NIL:
				case Variant::BOOL:
				case Variant::INT:
				case Variant::FLOAT:
				case Variant::STRING:
					return type;
				default:
					return Variant::NIL;
			}
		}
		default:
			return Variant::NIL;
	}
}

String GritGdxEmitter::convert(const String &p_expression, Variant::Type p_from, Variant::Type p_to) const {
	if (p_from == p_to) {
		return p_expression;
	}
	if (p_to == Variant::NIL) {
		return p_from == Variant::NIL ? p_expression : "Variant(" + p_expression + ")";
	}
	if (p_to == Variant::OBJECT) {
		return "static_cast<Object *>(" + p_expression + ")";
	}
	return "static_cast<" + cpp_storage(p_to) + ">(" + p_expression + ")";
}

String GritGdxEmitter::default_literal(const Variant &p_value, Variant::Type p_storage) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			return String(p_value.operator bool() ? "true" : "false");
		case Variant::INT:
			return "int64_t(" + itos(p_value.operator int64_t()) + ")";
		case Variant::FLOAT:
			return float_literal(p_value.operator double());
		case Variant::STRING:
			return "String(" + escaped_string(p_value.operator String()) + ")";
		case Variant::NIL:
			return "Variant()";
		case Variant::ARRAY: {
			const Array array = p_value;
			String items;
			for (int i = 0; i < array.size(); i++) {
				if (i > 0) {
					items += ", ";
				}
				const Variant &item = array[i];
				const String literal = default_literal(item, Variant::NIL);
				if (literal.is_empty()) {
					return String();
				}
				items += literal;
			}
			return "Variant(grit_gdx::array({ " + items + " }))";
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			String items;
			bool first = true;
			for (const KeyValue<Variant, Variant> &entry : dictionary) {
				const String key = default_literal(entry.key, Variant::NIL);
				const String value = default_literal(entry.value, Variant::NIL);
				if (key.is_empty() || value.is_empty()) {
					return String();
				}
				if (!first) {
					items += ", ";
				}
				items += key + ", " + value;
				first = false;
			}
			return "Variant(grit_gdx::dictionary({ " + items + " }))";
		}
		default:
			return String();
	}
}

String GritGdxEmitter::constant_expression(int p_constant) {
	const Variant::Type type = function->constant_types[p_constant];
	const Variant &value = p_constant < int(function->constant_values.size()) ? function->constant_values[p_constant] : function->constants[p_constant];
	switch (type) {
		case Variant::NIL:
			return "Variant()";
		case Variant::BOOL:
			return value.operator bool() ? "true" : "false";
		case Variant::INT:
			return "int64_t(" + itos(value.operator int64_t()) + ")";
		case Variant::FLOAT:
			return float_literal(value.operator double());
		case Variant::STRING:
			return "String(" + escaped_string(value.operator String()) + ")";
		case Variant::STRING_NAME:
			return "StringName(String(" + escaped_string(value.operator String()) + "))";
		case Variant::NODE_PATH:
			return "NodePath(String(" + escaped_string(value.operator String()) + "))";
		case Variant::OBJECT: {
			// Singleton references (`Time`, `Engine`, `Input`, …) arrive as
			// object constants; their runtime ids differ, so call the generated
			// accessor instead.
			const Object *object = value;
			if (!object) {
				return "Variant()";
			}
			const StringName class_name = object->get_class_name();
			if (ClassDB::class_exists(class_name) && Engine::get_singleton()->has_singleton(class_name)) {
				return "Variant(" + String(class_name) + "::get_singleton())";
			}
			reject(vformat("constant object of type %s", class_name));
			return "Variant()";
		}
		default:
			// Everything else (vectors, colors, transforms, arrays, dictionaries
			// and the rest) is rebuilt from its literal form, which is what the
			// value would print as anyway. Constants are rare enough that the
			// parse is cheaper than a case for every built-in type.
			return "UtilityFunctions::str_to_var(String(" + escaped_string(value.stringify()) + "))";
	}
}

String GritGdxEmitter::read(const GritOperand &p_operand) {
	switch (p_operand.kind) {
		case GritOperand::Kind::VARIABLE:
			return variable_name(p_operand.index);
		case GritOperand::Kind::SELF:
			return "this";
		case GritOperand::Kind::CONSTANT:
			return constant_expression(p_operand.index);
		case GritOperand::Kind::MEMBER: {
			const GritGdxProperty *property = property_of(p_operand.index);
			if (!property) {
				reject("member access with an unknown index");
				return "Variant()";
			}
			return property->field;
		}
		case GritOperand::Kind::NIL:
			return "Variant()";
		case GritOperand::Kind::CLASS:
			reject("class reference");
			return "Variant()";
	}
	return "Variant()";
}

String GritGdxEmitter::read_as(const GritOperand &p_operand, Variant::Type p_type) {
	return convert(read(p_operand), storage_of(p_operand), p_type);
}

String GritGdxEmitter::read_any(const GritOperand &p_operand) {
	const Variant::Type storage = storage_of(p_operand);
	const String expression = read(p_operand);
	return storage == Variant::NIL ? expression : "Variant(" + expression + ")";
}

String GritGdxEmitter::lvalue(const GritOperand &p_operand) {
	switch (p_operand.kind) {
		case GritOperand::Kind::VARIABLE:
			return variable_name(p_operand.index);
		case GritOperand::Kind::MEMBER: {
			const GritGdxProperty *property = property_of(p_operand.index);
			if (!property) {
				reject("member access with an unknown index");
				return "grit_invalid_lvalue";
			}
			return property->field;
		}
		default:
			reject("assignment to a value that is not a variable");
			return "grit_invalid_lvalue";
	}
}

String GritGdxEmitter::dynamic_operator(Variant::Operator p_operator, const GritOperand &p_left, const GritOperand &p_right) {
	const char *code = operator_code(p_operator);
	if (!code) {
		reject("dynamic operator");
		return "Variant()";
	}
	return String("grit_gdx::binary(") + code + ", " + read_any(p_left) + ", " + read_any(p_right) + ")";
}

String GritGdxEmitter::call_arguments(const GritStatement &p_statement, int p_first, const GritFunction *p_target) {
	String arguments;
	int parameter = 0;
	for (int i = p_first; i < int(p_statement.operands.size()); i++, parameter++) {
		if (parameter > 0) {
			arguments += ", ";
		}
		Variant::Type wanted = Variant::NIL;
		if (p_target) {
			int seen = 0;
			for (const GritVariable &variable : p_target->variables) {
				if (variable.role != GritVariable::Role::PARAMETER) {
					continue;
				}
				if (seen == parameter) {
					wanted = variable.storage;
					break;
				}
				seen++;
			}
		}
		arguments += read_as(p_statement.operands[i], wanted);
	}
	return arguments;
}

bool GritGdxEmitter::write_operator(const GritStatement &p_statement) {
	const GritOperand &target = p_statement.target;

	// TEST is the truthiness conversion GDScript uses for `and`/`or` and
	// conditions; like UNARY it carries a single operand.
	if (p_statement.opcode == GritOpcode::TEST) {
		const GritOperand &operand_operand = p_statement.operands[0];
		const String operand = read(operand_operand);
		const Variant::Type storage = storage_of(operand_operand);
		const String truth = storage == Variant::BOOL ? operand : "Variant(" + operand + ").booleanize()";
		line(lvalue(target) + " = " + convert(truth, Variant::BOOL, storage_of(target)) + ";");
		return true;
	}

	if (p_statement.opcode == GritOpcode::UNARY) {
		const GritOperand &operand_operand = p_statement.operands[0];
		const String operand = read(operand_operand);
		const Variant::Type storage = storage_of(operand_operand);
		String expression;
		switch (p_statement.op) {
			case Variant::OP_NEGATE:
				expression = "-" + operand;
				break;
			case Variant::OP_POSITIVE:
				expression = "(" + operand + ")";
				break;
			case Variant::OP_NOT: {
				const String truth = storage == Variant::NIL ? "Variant(" + operand + ").booleanize()" : operand;
				expression = "!(" + truth + ")";
				break;
			}
			case Variant::OP_BIT_NEGATE:
				expression = "~" + operand;
				break;
			default:
				reject("unary operator");
				return false;
		}
		line(lvalue(target) + " = " + convert(expression, storage, storage_of(target)) + ";");
		return true;
	}

	const GritOperand &left_operand = p_statement.operands[0];
	const GritOperand &right_operand = p_statement.operands[1];
	const Variant::Type left_storage = storage_of(left_operand);
	const Variant::Type right_storage = storage_of(right_operand);
	const Variant::Type target_storage = storage_of(target);

	String op;
	switch (p_statement.op) {
		case Variant::OP_ADD:
			op = "+";
			break;
		case Variant::OP_SUBTRACT:
			op = "-";
			break;
		case Variant::OP_MULTIPLY:
			op = "*";
			break;
		case Variant::OP_DIVIDE:
			op = "/";
			break;
		case Variant::OP_MODULE:
			op = "%";
			break;
		case Variant::OP_EQUAL:
			op = "==";
			break;
		case Variant::OP_NOT_EQUAL:
			op = "!=";
			break;
		case Variant::OP_LESS:
			op = "<";
			break;
		case Variant::OP_LESS_EQUAL:
			op = "<=";
			break;
		case Variant::OP_GREATER:
			op = ">";
			break;
		case Variant::OP_GREATER_EQUAL:
			op = ">=";
			break;
		case Variant::OP_AND:
			op = "&";
			break;
		case Variant::OP_OR:
			op = "|";
			break;
		case Variant::OP_XOR:
			op = "^";
			break;
		case Variant::OP_SHIFT_LEFT:
			op = "<<";
			break;
		case Variant::OP_SHIFT_RIGHT:
			op = ">>";
			break;
		default:
			reject("operator " + Variant::get_operator_name(p_statement.op));
			return false;
	}

	String expression;
	if (left_storage == Variant::NIL || right_storage == Variant::NIL) {
		expression = dynamic_operator(p_statement.op, left_operand, right_operand);
		line(lvalue(target) + " = " + convert(expression, Variant::NIL, target_storage) + ";");
		return true;
	}

	if ((left_storage == Variant::INT || left_storage == Variant::FLOAT) && (right_storage == Variant::INT || right_storage == Variant::FLOAT) && left_storage != right_storage) {
		expression = "(" + convert(read(left_operand), left_storage, Variant::FLOAT) + " " + op + " " + convert(read(right_operand), right_storage, Variant::FLOAT) + ")";
	} else {
		expression = "(" + read(left_operand) + " " + op + " " + read(right_operand) + ")";
	}
	line(lvalue(target) + " = " + convert(expression, target_storage, target_storage) + ";");
	return true;
}

bool GritGdxEmitter::write_access(const GritStatement &p_statement) {
	switch (p_statement.opcode) {
		case GritOpcode::GET_KEYED: {
			const GritOperand &container = p_statement.operands[0];
			const String key = read_as(p_statement.operands[1], Variant::INT);
			const Variant::Type container_storage = storage_of(container);
			String expression;
			if (container_storage == Variant::ARRAY) {
				expression = read(container) + "[" + key + "]";
			} else if (container_storage == Variant::DICTIONARY) {
				expression = read(container) + "[" + read_any(p_statement.operands[1]) + "]";
			} else if (container_storage == Variant::STRING) {
				expression = read(container) + "[" + key + "]";
			} else {
				expression = "grit_gdx::get_keyed(" + read_any(container) + ", " + read_any(p_statement.operands[1]) + ")";
			}
			line(lvalue(p_statement.target) + " = " + convert(expression, Variant::NIL, storage_of(p_statement.target)) + ";");
			return true;
		}
		case GritOpcode::SET_KEYED: {
			const GritOperand &container = p_statement.operands[0];
			const Variant::Type container_storage = storage_of(container);
			if (container_storage == Variant::ARRAY) {
				line(read(container) + "[" + read_as(p_statement.operands[1], Variant::INT) + "] = " + read_any(p_statement.operands[2]) + ";");
			} else if (container_storage == Variant::DICTIONARY) {
				line(read(container) + "[" + read_any(p_statement.operands[1]) + "] = " + read_any(p_statement.operands[2]) + ";");
			} else {
				line("grit_gdx::set_keyed(" + read_any(container) + ", " + read_any(p_statement.operands[1]) + ", " + read_any(p_statement.operands[2]) + ");");
			}
			return true;
		}
		case GritOpcode::GET_PROPERTY: {
			const String name = String(p_statement.name);
			const GritGdxProperty *const *property = properties_by_name.getptr(name);
			String expression;
			if (property) {
				expression = (*property)->field;
			} else {
				expression = "this->get(StringName(" + escaped_string(name) + "))";
			}
			line(lvalue(p_statement.target) + " = " + convert(expression, property ? (*property)->storage : Variant::NIL, storage_of(p_statement.target)) + ";");
			return true;
		}
		case GritOpcode::SET_PROPERTY: {
			const String name = String(p_statement.name);
			const GritGdxProperty *const *property = properties_by_name.getptr(name);
			if (property) {
				line((*property)->field + " = " + read_as(p_statement.operands[0], (*property)->storage) + ";");
			} else {
				line("this->set(StringName(" + escaped_string(name) + "), " + read_any(p_statement.operands[0]) + ");");
			}
			return true;
		}
		case GritOpcode::GET_NAMED: {
			const String base = read_any(p_statement.operands[0]);
			const String expression = "grit_gdx::get_named(" + base + ", StringName(" + escaped_string(String(p_statement.name)) + "))";
			line(lvalue(p_statement.target) + " = " + convert(expression, Variant::NIL, storage_of(p_statement.target)) + ";");
			return true;
		}
		case GritOpcode::SET_NAMED: {
			line("grit_gdx::set_named(" + read_any(p_statement.operands[0]) + ", StringName(" + escaped_string(String(p_statement.name)) + "), " + read_any(p_statement.operands[1]) + ");");
			return true;
		}
		case GritOpcode::CONSTRUCT: {
			const String type = cpp_storage(p_statement.builtin_type);
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			line(lvalue(p_statement.target) + " = " + type + "(" + arguments + ");");
			return true;
		}
		case GritOpcode::CONSTRUCT_ARRAY: {
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			line(lvalue(p_statement.target) + " = grit_gdx::array({ " + arguments + " });");
			return true;
		}
		case GritOpcode::CONSTRUCT_DICTIONARY: {
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			line(lvalue(p_statement.target) + " = grit_gdx::dictionary({ " + arguments + " });");
			return true;
		}
		default:
			reject("access opcode");
			return false;
	}
}

bool GritGdxEmitter::write_return(const GritStatement &p_statement) {
	const GritType &return_type = function->types[function->return_type];
	if (coroutine) {
		// The step method always yields a Variant; the value only reaches a
		// resumed GDScript caller through the completion signal, which a
		// GDExtension entry cannot produce, so it is returned to nobody.
		line("p_state->ip = 0;");
		if (return_type.kind == GritType::Kind::BUILTIN && return_type.builtin_type == Variant::NIL) {
			line("return Variant();");
		} else {
			line("return Variant(" + read_any(p_statement.operands[0]) + ");");
		}
		return true;
	}
	if (return_type.kind == GritType::Kind::BUILTIN && return_type.builtin_type == Variant::NIL) {
		line("return;");
		return true;
	}
	if (return_type.kind == GritType::Kind::VARIANT) {
		line("return " + read_any(p_statement.operands[0]) + ";");
		return true;
	}
	if (return_type.kind == GritType::Kind::BUILTIN) {
		line("return " + read_as(p_statement.operands[0], return_type.builtin_type) + ";");
		return true;
	}
	line("return " + read_any(p_statement.operands[0]) + ";");
	return true;
}

bool GritGdxEmitter::write_call(const GritStatement &p_statement) {
	const GritOperand &target = p_statement.target;
	const String name = String(p_statement.name);

	switch (p_statement.opcode) {
		case GritOpcode::CALL_UTILITY: {
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			const String expression = "UtilityFunctions::" + name + "(" + arguments + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_GDSCRIPT_UTILITY: {
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			String expression;
			if (!is_gdscript_utility(name, arguments, expression)) {
				reject(vformat("GDScript utility function \"%s\"", name));
				return false;
			}
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL:
		case GritOpcode::CALL_ASYNC: {
			const GritOperand &base = p_statement.operands[0];
			const GritFunction *method = method_named(name);
			const bool self_call = base.kind == GritOperand::Kind::SELF && method && is_supported(name);
			String expression;
			if (self_call) {
				expression = "this->" + sanitize(name) + "(" + call_arguments(p_statement, 1, method) + ")";
			} else {
				String arguments;
				for (uint32_t i = 1; i < p_statement.operands.size(); i++) {
					if (i > 1) {
						arguments += ", ";
					}
					arguments += read_any(p_statement.operands[i]);
				}
				const String base_expression = base.kind == GritOperand::Kind::SELF ? String("Variant(this)") : read_any(base);
				expression = "grit_gdx::call_method(" + base_expression + ", StringName(" + escaped_string(name) + ")" + (arguments.is_empty() ? String() : ", " + arguments) + ")";
			}
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else if (self_call && return_type_of(*method) == "void") {
				// A void method (a coroutine counts: its entry returns nothing)
				// has no value to hand on, so the result slot is cleared.
				line(expression + ";");
				line(lvalue(target) + " = " + default_storage_value(storage_of(target)) + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_NATIVE_STATIC: {
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			const String expression = String(p_statement.class_name) + "::" + name + "(" + arguments + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_METHOD_BIND: {
			String arguments;
			for (uint32_t i = 1; i < p_statement.operands.size(); i++) {
				if (i > 1) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			const String expression = "grit_gdx::call_method(" + read_any(p_statement.operands[0]) + ", StringName(" + escaped_string(name) + ")" + (arguments.is_empty() ? String() : ", " + arguments) + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_BUILTIN: {
			String arguments;
			for (uint32_t i = 1; i < p_statement.operands.size(); i++) {
				if (i > 1) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			const String expression = "grit_gdx::call_method(" + read_any(p_statement.operands[0]) + ", StringName(" + escaped_string(name) + ")" + (arguments.is_empty() ? String() : ", " + arguments) + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_SUPER: {
			// The generated class derives from a native class (a script parent is
			// refused at generation time), so super reaches the native base.
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				arguments += read_any(p_statement.operands[i]);
			}
			const String expression = String(klass.base_type) + "::" + name + "(" + arguments + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CALL_BUILTIN_STATIC: {
			// Static methods of built-in types (`Color.from_hsv(…)`).
			const Variant::Type builtin = p_statement.builtin_type;
			const String type = cpp_storage(builtin);
			const bool vararg = Variant::is_builtin_method_vararg(builtin, name);
			String arguments;
			for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
				if (i > 0) {
					arguments += ", ";
				}
				const Variant::Type wanted = vararg ? Variant::NIL : Variant::get_builtin_method_argument_type(builtin, name, i);
				arguments += read_as(p_statement.operands[i], wanted);
			}
			const String expression = type + "::" + name + "(" + arguments + ")";
			if (target.kind == GritOperand::Kind::NIL) {
				line(expression + ";");
			} else {
				line(lvalue(target) + " = " + convert(expression, Variant::NIL, storage_of(target)) + ";");
			}
			return true;
		}
		case GritOpcode::CREATE_LAMBDA:
			reject(vformat("unsupported call form: %s", name));
			return false;
		default:
			reject("unsupported call");
			return false;
	}
}

bool GritGdxEmitter::write_control(const GritStatement &p_statement) {
	switch (p_statement.opcode) {
		case GritOpcode::IF: {
			const Variant::Type condition_storage = storage_of(p_statement.operands[0]);
			const String condition = condition_storage == Variant::BOOL ? read(p_statement.operands[0]) : read_any(p_statement.operands[0]) + ".booleanize()";
			line("if (" + condition + ") {");
			indentation++;
			if (!write_block(p_statement.blocks[0])) {
				return false;
			}
			indentation--;
			if (p_statement.blocks[1] >= 0 && !function->blocks[p_statement.blocks[1]].statements.is_empty()) {
				line("} else {");
				indentation++;
				if (!write_block(p_statement.blocks[1])) {
					return false;
				}
				indentation--;
			}
			line("}");
			return true;
		}
		case GritOpcode::WHILE: {
			line("for (;;) {");
			indentation++;
			if (!write_block(p_statement.blocks[0])) {
				return false;
			}
			const Variant::Type condition_storage = storage_of(p_statement.operands[0]);
			const String condition = condition_storage == Variant::BOOL ? read(p_statement.operands[0]) : read_any(p_statement.operands[0]) + ".booleanize()";
			line("if (!(" + condition + ")) { break; }");
			if (!write_block(p_statement.blocks[1])) {
				return false;
			}
			indentation--;
			line("}");
			return true;
		}
		case GritOpcode::FOR_RANGE: {
			const String from = read_as(p_statement.operands[0], Variant::INT);
			const String to = read_as(p_statement.operands[1], Variant::INT);
			const String step = read_as(p_statement.operands[2], Variant::INT);
			const String from_base = "range_from_" + itos(symbol_count);
			const String to_base = "range_to_" + itos(symbol_count);
			const String step_base = "range_step_" + itos(symbol_count);
			const String counter_base = "range_counter_" + itos(symbol_count);
			symbol_count++;
			String from_symbol = from_base;
			String to_symbol = to_base;
			String step_symbol = step_base;
			String counter = counter_base;
			line("{");
			indentation++;
			if (coroutine) {
				// Inside a coroutine the loop state has to survive an await, so
				// it lives in the state object and the loop header only runs
				// assignments (a resumption jumps straight into the body).
				add_state_field("int64_t", from_base, "0");
				add_state_field("int64_t", to_base, "0");
				add_state_field("int64_t", step_base, "0");
				add_state_field("int64_t", counter_base, "0");
				from_symbol = state_reference(from_base);
				to_symbol = state_reference(to_base);
				step_symbol = state_reference(step_base);
				counter = state_reference(counter_base);
				line(from_symbol + " = " + from + ";");
				line(to_symbol + " = " + to + ";");
				line(step_symbol + " = " + step + ";");
				line("for (" + counter + " = " + from_symbol + "; " + step_symbol + " != 0 && (" + step_symbol + " > 0 ? " + counter + " < " + to_symbol + " : " + counter + " > " + to_symbol + "); " + counter + " += " + step_symbol + ") {");
			} else {
				line("const int64_t " + from_symbol + " = " + from + ";");
				line("const int64_t " + to_symbol + " = " + to + ";");
				line("const int64_t " + step_symbol + " = " + step + ";");
				line("for (int64_t " + counter + " = " + from_symbol + "; " + step_symbol + " != 0 && (" + step_symbol + " > 0 ? " + counter + " < " + to_symbol + " : " + counter + " > " + to_symbol + "); " + counter + " += " + step_symbol + ") {");
			}
			indentation++;
			line(lvalue(p_statement.target) + " = " + convert(counter, Variant::INT, storage_of(p_statement.target)) + ";");
			if (!write_block(p_statement.blocks[0])) {
				return false;
			}
			indentation--;
			line("}");
			indentation--;
			line("}");
			return true;
		}
		case GritOpcode::FOR_EACH: {
			const GritOperand &container = p_statement.operands[0];
			const Variant::Type container_storage = storage_of(container);
			const String container_base = "foreach_container_" + itos(symbol_count);
			const String index_base = "foreach_index_" + itos(symbol_count);
			symbol_count++;
			String container_type;
			String container_value;
			if (container_storage == Variant::STRING) {
				container_type = "String";
				container_value = read(container);
			} else if (container_storage == Variant::ARRAY) {
				container_type = "Array";
				container_value = read(container);
			} else if (container_storage == Variant::INT) {
				// `for i in 3` iterates a count, not a range.
				container_type = "int64_t";
				container_value = read_as(container, Variant::INT);
			} else if (container_storage == Variant::NIL || container_storage == Variant::DICTIONARY) {
				container_type = "Array";
				container_value = "grit_gdx::iterate(" + read_any(container) + ")";
			} else {
				reject(vformat("for over this container type (%s)", Variant::get_type_name(container_storage)));
				return false;
			}
			String container_symbol = container_base;
			String index_symbol = index_base;
			line("{");
			indentation++;
			if (coroutine) {
				add_state_field(container_type, container_base, container_type + "()");
				add_state_field("int64_t", index_base, "0");
				container_symbol = state_reference(container_base);
				index_symbol = state_reference(index_base);
				line(container_symbol + " = " + container_value + ";");
			} else {
				line("const " + container_type + " " + container_symbol + " = " + container_value + ";");
			}
			// An int container is its own upper bound; arrays, strings and
			// dictionary keys all expose a size.
			String size_expression = container_symbol;
			if (container_storage == Variant::STRING) {
				size_expression = container_symbol + ".length()";
			} else if (container_storage != Variant::INT) {
				size_expression = container_symbol + ".size()";
			}
			if (coroutine) {
				line("for (" + index_symbol + " = 0; " + index_symbol + " < " + size_expression + "; " + index_symbol + "++) {");
			} else {
				line("for (int64_t " + index_symbol + " = 0; " + index_symbol + " < " + size_expression + "; " + index_symbol + "++) {");
			}
			indentation++;
			const String element = container_storage == Variant::INT ? index_symbol : container_symbol + "[" + index_symbol + "]";
			line(lvalue(p_statement.target) + " = " + convert(element, Variant::NIL, storage_of(p_statement.target)) + ";");
			if (!write_block(p_statement.blocks[0])) {
				return false;
			}
			indentation--;
			line("}");
			indentation--;
			line("}");
			return true;
		}
		case GritOpcode::BREAK:
			line("break;");
			return true;
		case GritOpcode::CONTINUE:
			line("continue;");
			return true;
		case GritOpcode::RETURN:
			return write_return(p_statement);
		default:
			reject("unsupported control opcode");
			return false;
	}
}

bool GritGdxEmitter::write_await(const GritStatement &p_statement) {
	if (!coroutine) {
		reject("await outside a coroutine");
		return false;
	}
	if (p_statement.operands.is_empty()) {
		reject("await without an operand");
		return false;
	}
	const int resume_point = p_statement.index + 1;
	const auto assign_target = [&](const String &p_value) {
		if (p_statement.target.kind == GritOperand::Kind::NIL) {
			return;
		}
		const Variant::Type storage = storage_of(p_statement.target);
		line(lvalue(p_statement.target) + " = " + convert(p_value, Variant::NIL, storage) + ";");
	};

	line("{");
	indentation++;
	line("Variant await_operand = " + read_any(p_statement.operands[0]) + ";");
	line("Signal await_signal;");
	line("Variant await_value;");
	line("const int await_action = grit_gdx::await_prepare(await_operand, await_signal, await_value);");
	line("if (await_action < 0) {");
	indentation++;
	line("p_state->ip = 0;");
	line("return Variant();");
	indentation--;
	line("}");
	line("if (await_action == 0) {");
	indentation++;
	// Not a signal (a coroutine that already completed, a plain value, …):
	// GDScript continues immediately with that value.
	assign_target("await_value");
	indentation--;
	line("} else {");
	indentation++;
	line("p_state->awaited = Variant();");
	line("p_state->ip = " + itos(resume_point) + ";");
	line(state_map_name + "[p_state->handle] = p_state;");
	line("const Callable await_callback(memnew(grit_gdx::AwaitCallback(this, p_state->handle, StringName(" + escaped_string(state_resume_method) + "))));");
	line("if (await_signal.connect(await_callback, Object::CONNECT_ONE_SHOT) != 0) {");
	indentation++;
	line("UtilityFunctions::push_error(\"Grit: could not connect to the awaited signal.\");");
	line("p_state->ip = 0;");
	line("return Variant();");
	indentation--;
	line("}");
	line("return Variant();");
	indentation--;
	line("}");
	indentation--;
	line("}");
	line("await_resume_" + itos(resume_point) + ":;");
	assign_target("p_state->awaited");
	return true;
}

// `assert`: GDScript compiles asserts only in debug builds and aborts the
// function when one fails, so the generated code does the same and is empty in
// release builds. The block that was split off by the front end evaluates the
// message before the test.
bool GritGdxEmitter::write_assert(const GritStatement &p_statement) {
	line("#ifdef DEBUG_ENABLED");
	indentation++;
	if (p_statement.blocks[0] >= 0) {
		if (!write_block(p_statement.blocks[0])) {
			return false;
		}
	}
	const Variant::Type condition_storage = storage_of(p_statement.operands[0]);
	const String condition = condition_storage == Variant::BOOL ? read(p_statement.operands[0]) : read_any(p_statement.operands[0]) + ".booleanize()";
	line("if (!(" + condition + ")) {");
	indentation++;
	if (p_statement.validated) {
		line("UtilityFunctions::push_error(String(\"Grit: Assertion failed: \") + String(" + read_any(p_statement.operands[1]) + "));");
	} else {
		line("UtilityFunctions::push_error(\"Grit: Assertion failed.\");");
	}
	write_abort_return();
	indentation--;
	line("}");
	indentation--;
	line("#endif");
	return true;
}

// Leaves the current function with its default value, the way a failed assert
// (or a returned void) does. Coroutines also clear their resume point.
void GritGdxEmitter::write_abort_return() {
	const String return_cpp = return_type_of(*function);
	if (coroutine) {
		line("p_state->ip = 0;");
		line("return Variant();");
		return;
	}
	if (return_cpp == "Variant") {
		line("return Variant();");
	} else if (return_cpp == "Object *") {
		line("return nullptr;");
	} else if (return_cpp == "void") {
		line("return;");
	} else {
		const GritType &type = function->types[function->return_type];
		line("return " + default_storage_value(type.kind == GritType::Kind::BUILTIN ? type.builtin_type : Variant::NIL) + ";");
	}
}

// `is`: a builtin type compares the Variant type, a native type asks the object
// for its class chain. Script types are still refused: the IR only carries the
// class-path string, so the check could not tell two inner classes apart.
bool GritGdxEmitter::write_type_test(const GritStatement &p_statement) {
	const GritType &type = function->types[p_statement.type];
	const String value = read_any(p_statement.operands[0]);
	String expression;
	switch (type.kind) {
		case GritType::Kind::BUILTIN: {
			if (type.has_elements() && (type.builtin_type == Variant::ARRAY || type.builtin_type == Variant::DICTIONARY)) {
				if (!container_type_test(type, value, expression)) {
					return false;
				}
			} else {
				expression = "(" + value + ".get_type() == Variant::" + variant_enum(type.builtin_type) + ")";
			}
		} break;
		case GritType::Kind::NATIVE:
			expression = "grit_gdx::is_native(" + value + ", StringName(" + escaped_string(String(type.native_type)) + "))";
			break;
		case GritType::Kind::SCRIPT:
			reject("'is' with a script type");
			return false;
		case GritType::Kind::VARIANT:
			expression = "false";
			break;
	}
	line(lvalue(p_statement.target) + " = " + convert(expression, Variant::BOOL, storage_of(p_statement.target)) + ";");
	return true;
}

bool GritGdxEmitter::container_type_test(const GritType &p_type, const String &p_value, String &r_expression) {
	const auto element_argument = [&](const GritType &p_element, String &r_builtin, String &r_native) {
		if (p_element.kind == GritType::Kind::SCRIPT) {
			reject("'is' with a script-typed container element");
			return false;
		}
		const Variant::Type builtin = p_element.kind == GritType::Kind::NATIVE ? Variant::OBJECT : p_element.builtin_type;
		r_builtin = "Variant::" + variant_enum(builtin);
		r_native = "StringName(" + escaped_string(p_element.kind == GritType::Kind::NATIVE ? String(p_element.native_type) : String()) + ")";
		return true;
	};
	if (p_type.builtin_type == Variant::ARRAY) {
		String builtin;
		String native;
		if (!element_argument(p_type.elements[0], builtin, native)) {
			return false;
		}
		r_expression = "grit_gdx::is_typed_array(" + p_value + ", " + builtin + ", " + native + ")";
		return true;
	}
	String key_builtin;
	String key_native;
	String value_builtin;
	String value_native;
	if (!element_argument(p_type.elements[0], key_builtin, key_native) || !element_argument(p_type.elements[1], value_builtin, value_native)) {
		return false;
	}
	r_expression = "grit_gdx::is_typed_dictionary(" + p_value + ", " + key_builtin + ", " + key_native + ", " + value_builtin + ", " + value_native + ")";
	return true;
}

// `as`: a builtin cast converts through the engine's own conversion (so a bad
// value keeps the GDScript result), an object cast yields null when the object
// is not of that class.
bool GritGdxEmitter::write_cast(const GritStatement &p_statement) {
	const GritType &type = function->types[p_statement.type];
	const String value = read_any(p_statement.operands[0]);
	String expression;
	switch (type.kind) {
		case GritType::Kind::BUILTIN:
			expression = "grit_gdx::cast_builtin(" + value + ", " + "Variant::" + variant_enum(type.builtin_type) + ")";
			break;
		case GritType::Kind::NATIVE:
			expression = "grit_gdx::cast_native(" + value + ", StringName(" + escaped_string(String(type.native_type)) + "))";
			break;
		case GritType::Kind::SCRIPT:
			reject("'as' with a script type");
			return false;
		case GritType::Kind::VARIANT:
			expression = value;
			break;
	}
	line(lvalue(p_statement.target) + " = " + convert(expression, Variant::NIL, storage_of(p_statement.target)) + ";");
	return true;
}

bool GritGdxEmitter::write_statement(const GritStatement &p_statement) {
	switch (p_statement.opcode) {
		case GritOpcode::ASSIGN:
		case GritOpcode::ASSIGN_TYPED: {
			const Variant::Type storage = storage_of(p_statement.target);
			if (storage == Variant::OBJECT) {
				line(lvalue(p_statement.target) + " = " + convert(read_any(p_statement.operands[0]), Variant::NIL, Variant::OBJECT) + ";");
			} else {
				line(lvalue(p_statement.target) + " = " + read_as(p_statement.operands[0], storage) + ";");
			}
			return true;
		}
		case GritOpcode::CLEAR:
			line(lvalue(p_statement.target) + " = " + default_storage_value(storage_of(p_statement.target)) + ";");
			return true;
		case GritOpcode::TEST:
		case GritOpcode::UNARY:
		case GritOpcode::BINARY:
			return write_operator(p_statement);
		case GritOpcode::GET_KEYED:
		case GritOpcode::SET_KEYED:
		case GritOpcode::GET_NAMED:
		case GritOpcode::SET_NAMED:
		case GritOpcode::GET_PROPERTY:
		case GritOpcode::SET_PROPERTY:
		case GritOpcode::CONSTRUCT:
		case GritOpcode::CONSTRUCT_ARRAY:
		case GritOpcode::CONSTRUCT_DICTIONARY:
			return write_access(p_statement);
		case GritOpcode::CALL:
		case GritOpcode::CALL_ASYNC:
		case GritOpcode::CALL_SUPER:
		case GritOpcode::CALL_UTILITY:
		case GritOpcode::CALL_GDSCRIPT_UTILITY:
		case GritOpcode::CALL_BUILTIN:
		case GritOpcode::CALL_BUILTIN_STATIC:
		case GritOpcode::CALL_NATIVE_STATIC:
		case GritOpcode::CALL_METHOD_BIND:
		case GritOpcode::CREATE_LAMBDA:
			return write_call(p_statement);
		case GritOpcode::AWAIT:
			return write_await(p_statement);
		case GritOpcode::TYPE_TEST:
			return write_type_test(p_statement);
		case GritOpcode::CAST:
			return write_cast(p_statement);
		case GritOpcode::GET_STATIC:
		case GritOpcode::SET_STATIC:
		case GritOpcode::GET_GLOBAL:
		case GritOpcode::DEFAULT_ARGUMENT:
			reject("static or global access");
			return false;
		case GritOpcode::ASSERT:
			return write_assert(p_statement);
		case GritOpcode::IF_NOT_SHARED:
			reject("shared value branch");
			return false;
		default:
			return write_control(p_statement);
	}
}

bool GritGdxEmitter::write_block(int p_block) {
	for (const GritStatement &statement : function->blocks[p_block].statements) {
		if (!write_statement(statement) || !reject_reason.is_empty()) {
			return false;
		}
	}
	return true;
}

bool GritGdxEmitter::is_virtual_name(const String &p_name) {
	return find_virtual(p_name) != nullptr;
}

String GritGdxEmitter::virtual_signature_of(const GritFunction &p_function) {
	const VirtualInfo *info = find_virtual(String(p_function.name));
	if (!info) {
		return String();
	}
	String signature = String(info->return_type) + " " + sanitize(String(p_function.name)) + "(";
	for (int i = 0; i < info->param_count; i++) {
		if (i > 0) {
			signature += ", ";
		}
		signature += String(info->params[i].type) + " " + String(info->params[i].name);
	}
	return signature + ") override";
}

String GritGdxEmitter::function_return_cpp(const GritFunction &p_function) {
	const GritType &return_type = p_function.types[p_function.return_type];
	if (return_type.kind == GritType::Kind::VARIANT) {
		return "Variant";
	}
	if (return_type.kind == GritType::Kind::BUILTIN) {
		if (return_type.builtin_type == Variant::NIL) {
			return "void";
		}
		return cpp_storage(return_type.builtin_type);
	}
	return "Object *";
}

String GritGdxEmitter::return_type_of(const GritFunction &p_function) const {
	const VirtualInfo *info = is_virtual_name(String(p_function.name)) ? find_virtual(String(p_function.name)) : nullptr;
	return info ? String(info->return_type) : function_return_cpp(p_function);
}

String GritGdxEmitter::function_parameter_cpp(const GritVariable &p_variable, const String &p_name) {
	if (p_variable.storage == Variant::NIL) {
		return "const Variant &" + p_name;
	}
	return cpp_storage(p_variable.storage) + " " + p_name;
}

bool GritGdxEmitter::emit_function(const GritFunction &p_function, bool p_is_virtual, StringBuilder &r_output) {
	function = &p_function;
	reject_reason = String();
	body = StringBuilder();
	indentation = 1;
	symbol_count = 0;
	coroutine = p_function.await_count > 0;
	state_fields.clear();
	state_struct_name = await_struct_name(String(p_function.name));
	state_step_method = await_step_method(String(p_function.name));
	state_resume_method = await_resume_method(String(p_function.name));
	state_map_name = await_map_name(String(p_function.name));

	if (String(p_function.signature).begins_with("static ")) {
		reject("static functions are not supported");
		return false;
	}
	if (String(p_function.signature).contains(" optional ")) {
		reject("optional parameters are not supported");
		return false;
	}
	if (p_function.rest_variable >= 0) {
		reject("rest parameters are not supported");
		return false;
	}

	const VirtualInfo *virtual_info = p_is_virtual ? find_virtual(String(p_function.name)) : nullptr;
	LocalVector<int> parameter_variables;
	for (uint32_t i = 0; i < p_function.variables.size(); i++) {
		if (p_function.variables[i].role == GritVariable::Role::PARAMETER) {
			parameter_variables.push_back(i);
		}
	}
	if (virtual_info && int(parameter_variables.size()) != virtual_info->param_count) {
		reject(vformat("\"%s\" must take %d parameters", p_function.name, virtual_info->param_count));
		return false;
	}

	String signature_parameters;
	LocalVector<String> aliases;
	LocalVector<String> parameter_copies;
	if (virtual_info) {
		for (int i = 0; i < virtual_info->param_count; i++) {
			if (i > 0) {
				signature_parameters += ", ";
			}
			signature_parameters += String(virtual_info->params[i].type) + " " + String(virtual_info->params[i].name);
			const GritVariable &variable = p_function.variables[parameter_variables[i]];
			const String reference = variable_name(parameter_variables[i]);
			if (coroutine) {
				parameter_copies.push_back("grit_state->" + variable_base_name(parameter_variables[i]) + " = " + String(virtual_info->params[i].name) + ";");
			} else {
				aliases.push_back(cpp_storage(variable.storage) + " " + reference + " = " + String(virtual_info->params[i].name) + ";");
			}
		}
	} else {
		for (uint32_t i = 0; i < parameter_variables.size(); i++) {
			if (i > 0) {
				signature_parameters += ", ";
			}
			const GritVariable &variable = p_function.variables[parameter_variables[i]];
			const String parameter_name = "p_" + sanitize(variable.name);
			signature_parameters += function_parameter_cpp(variable, parameter_name);
			const String reference = variable_name(parameter_variables[i]);
			if (coroutine) {
				parameter_copies.push_back("grit_state->" + variable_base_name(parameter_variables[i]) + " = " + parameter_name + ";");
			} else {
				aliases.push_back(cpp_storage(variable.storage) + " " + reference + " = " + parameter_name + ";");
			}
		}
	}

	// A coroutine keeps every local (including loop state) in the heap object
	// that survives the suspension, so the state fields are collected before
	// the body decides to add more of them.
	if (coroutine) {
		for (uint32_t i = 0; i < p_function.variables.size(); i++) {
			const GritVariable &variable = p_function.variables[i];
			add_state_field(cpp_storage(variable.storage), variable_base_name(i), default_storage_value(variable.storage));
		}
	}

	// GDScript enables processing implicitly when a script defines _process; a
	// native override has to ask for it, and the class's own _ready is the place.
	if (String(p_function.name) == "_ready") {
		for (const GritFunction &other : klass.functions) {
			if (other.name == StringName("_process")) {
				line("set_process(true);");
			}
			if (other.name == StringName("_physics_process")) {
				line("set_physics_process(true);");
			}
		}
	}

	if (!write_block(0) || !reject_reason.is_empty()) {
		return false;
	}

	const String return_cpp = virtual_info ? String(virtual_info->return_type) : function_return_cpp(p_function);
	const String qualified_name = klass.class_name + "::" + sanitize(String(p_function.name));

	if (!coroutine) {
		r_output.append(return_cpp + " " + qualified_name + "(" + signature_parameters + ") {\n");
		for (const String &alias : aliases) {
			r_output.append("\t" + alias + "\n");
		}
		for (uint32_t i = 0; i < p_function.variables.size(); i++) {
			const GritVariable &variable = p_function.variables[i];
			if (variable.role == GritVariable::Role::PARAMETER) {
				continue;
			}
			r_output.append("\t" + cpp_storage(variable.storage) + " " + variable_name(i) + " = " + default_storage_value(variable.storage) + ";\n");
		}
		r_output.append(body.as_string());
		if (return_cpp == "Variant") {
			r_output.append("\treturn Variant();\n");
		} else if (return_cpp == "Object *") {
			r_output.append("\treturn nullptr;\n");
		} else if (return_cpp != "void") {
			const GritType &type = p_function.types[p_function.return_type];
			r_output.append("\treturn " + default_storage_value(type.kind == GritType::Kind::BUILTIN ? type.builtin_type : Variant::NIL) + ";\n");
		}
		r_output.append("}\n\n");
		await_state_fields.insert(p_function.name, state_fields);
		function = nullptr;
		return true;
	}

	// Entry point: allocate the state, copy the arguments in and run until the
	// first suspension. The caller sees a plain method call that returns at
	// once, the way a GDScript coroutine returns its function state.
	r_output.append(return_cpp + " " + qualified_name + "(" + signature_parameters + ") {\n");
	r_output.append("\t" + state_struct_name + " *grit_state = memnew(" + state_struct_name + ");\n");
	r_output.append("\tgrit_state->handle = ++grit_await_next;\n");
	for (const String &copy : parameter_copies) {
		r_output.append("\t" + copy + "\n");
	}
	r_output.append("\t" + state_step_method + "(grit_state);\n");
	r_output.append("\tif (grit_state->ip == 0) {\n\t\tmemdelete(grit_state);\n\t}\n");
	if (return_cpp == "Variant") {
		r_output.append("\treturn Variant();\n");
	} else if (return_cpp == "Object *") {
		r_output.append("\treturn nullptr;\n");
	} else if (return_cpp != "void") {
		const GritType &type = p_function.types[p_function.return_type];
		r_output.append("\treturn " + default_storage_value(type.kind == GritType::Kind::BUILTIN ? type.builtin_type : Variant::NIL) + ";\n");
	}
	r_output.append("}\n\n");

	// Step: jump to the suspension point that was recorded, then run until the
	// next await or the end. Every label lives in the same function body.
	r_output.append("Variant " + klass.class_name + "::" + state_step_method + "(" + state_struct_name + " *p_state) {\n");
	r_output.append("\tif (p_state->ip != 0) {\n");
	r_output.append("\t\tconst int64_t resume_point = p_state->ip;\n");
	r_output.append("\t\tp_state->ip = 0;\n");
	r_output.append("\t\tswitch (resume_point) {\n");
	for (int i = 1; i <= p_function.await_count; i++) {
		r_output.append("\t\t\tcase " + itos(i) + ": goto await_resume_" + itos(i) + ";\n");
	}
	r_output.append("\t\t\tdefault: return Variant();\n");
	r_output.append("\t\t}\n\t}\n");
	r_output.append(body.as_string());
	r_output.append("\treturn Variant();\n");
	r_output.append("}\n\n");

	// Resume: the awaited signal fired; hand its argument to the suspended step.
	r_output.append("void " + klass.class_name + "::" + state_resume_method + "(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error) {\n");
	r_output.append("\tr_error.error = GDEXTENSION_CALL_OK;\n");
	r_output.append("\tif (p_argcount < 1) {\n\t\tr_error.error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;\n\t\treturn;\n\t}\n");
	// AwaitCallback appends the state handle after the signal's own arguments,
	// so the awaited value is whatever the signal sent.
	r_output.append("\tconst GDExtensionInt value_count = p_argcount - 1;\n");
	r_output.append("\tconst int64_t handle = *p_args[value_count];\n");
	r_output.append("\t" + state_struct_name + " **entry = " + state_map_name + ".getptr(handle);\n");
	r_output.append("\tif (!entry) {\n\t\treturn;\n\t}\n");
	r_output.append("\t" + state_struct_name + " *state = *entry;\n");
	r_output.append("\t" + state_map_name + ".erase(handle);\n");
	r_output.append("\tif (value_count == 1) {\n");
	r_output.append("\t\tstate->awaited = *p_args[0];\n");
	r_output.append("\t} else if (value_count > 1) {\n");
	r_output.append("\t\tArray values;\n");
	r_output.append("\t\tfor (GDExtensionInt i = 0; i < value_count; i++) {\n");
	r_output.append("\t\t\tvalues.push_back(*p_args[i]);\n");
	r_output.append("\t\t}\n");
	r_output.append("\t\tstate->awaited = values;\n");
	r_output.append("\t} else {\n");
	r_output.append("\t\tstate->awaited = Variant();\n");
	r_output.append("\t}\n");
	r_output.append("\t" + state_step_method + "(state);\n");
	r_output.append("\tif (state->ip == 0) {\n\t\tmemdelete(state);\n\t}\n");
	r_output.append("}\n\n");

	await_state_fields.insert(p_function.name, state_fields);
	function = nullptr;
	return true;
}

String GritGdxEmitter::header_file_name(const GritGdxClass &p_class) {
	return snake_case(p_class.class_name) + ".h";
}

String GritGdxEmitter::emit_header(const GritGdxClass &p_class) {
	StringBuilder out;
	out.append("#pragma once\n\n");
	out.append("#include <godot_cpp/classes/" + snake_case(String(p_class.base_type)) + ".hpp>\n");
	out.append("#include <godot_cpp/core/class_db.hpp>\n");
	out.append("#include <godot_cpp/templates/hash_map.hpp>\n");
	out.append("#include <godot_cpp/variant/variant.hpp>\n");
	out.append("#include <godot_cpp/variant/string.hpp>\n");
	out.append("#include <godot_cpp/variant/string_name.hpp>\n");
	out.append("#include <godot_cpp/variant/node_path.hpp>\n");
	out.append("#include <godot_cpp/variant/array.hpp>\n");
	out.append("#include <godot_cpp/variant/dictionary.hpp>\n");
	out.append("#include <godot_cpp/variant/color.hpp>\n");
	out.append("#include <godot_cpp/variant/callable.hpp>\n");
	out.append("#include <godot_cpp/variant/signal.hpp>\n");
	out.append("#include <godot_cpp/variant/vector2.hpp>\n");
	out.append("#include <godot_cpp/variant/vector3.hpp>\n");
	out.append("#include <godot_cpp/variant/transform2d.hpp>\n");
	out.append("#include <godot_cpp/variant/transform3d.hpp>\n\n");
	out.append("#include \"grit_gdx_helpers.h\"\n\n");
	out.append("namespace godot {\n\n");

	GritGdxEmitter emitter(p_class);
	emitter.prepare();
	bool has_coroutines = false;
	for (const GritFunction &p_function : p_class.functions) {
		if (p_function.await_count > 0 && emitter.is_supported(String(p_function.name))) {
			has_coroutines = true;
			break;
		}
	}

	out.append("class " + p_class.class_name + " : public " + String(p_class.base_type) + " {\n");
	out.append("\tGDCLASS(" + p_class.class_name + ", " + String(p_class.base_type) + ")\n\n");

	for (const GritGdxProperty &property : p_class.properties) {
		String initializer;
		if (property.has_default) {
			if (property.storage == Variant::NIL) {
				initializer = default_literal(property.default_value, Variant::NIL);
			} else {
				initializer = default_literal(property.default_value, property.storage);
			}
		}
		if (initializer.is_empty()) {
			initializer = default_storage_value(property.storage);
		}
		out.append("\t" + cpp_storage(property.storage) + " " + property.field + " = " + initializer + ";\n");
	}

	out.append("\npublic:\n\tstatic void _bind_methods();\n\n");
	if (has_coroutines) {
		out.append("\t~" + p_class.class_name + "();\n\n");
	}
	for (const GritGdxProperty &property : p_class.properties) {
		const String type = cpp_storage(property.storage);
		out.append("\t" + type + " " + property.getter + "() const;\n");
		out.append("\tvoid " + property.setter + "(" + (property.storage == Variant::NIL ? String("const Variant &p_value") : type + " p_value") + ");\n");
	}
	out.append("\n");

	bool has_ready = emitter.is_supported("_ready");
	for (const GritFunction &p_function : p_class.functions) {
		const bool is_virtual = is_virtual_name(String(p_function.name));
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		if (is_virtual) {
			out.append("\t" + virtual_signature_of(p_function) + ";\n");
			continue;
		}
		String parameters;
		int count = 0;
		for (const GritVariable &variable : p_function.variables) {
			if (variable.role != GritVariable::Role::PARAMETER) {
				continue;
			}
			if (count > 0) {
				parameters += ", ";
			}
			parameters += function_parameter_cpp(variable, "p_" + sanitize(variable.name));
			count++;
		}
		out.append("\t" + function_return_cpp(p_function) + " " + sanitize(String(p_function.name)) + "(" + parameters + ");\n");
	}

	bool needs_process = false;
	for (const GritFunction &p_function : p_class.functions) {
		if (p_function.name == StringName("_process") || p_function.name == StringName("_physics_process")) {
			needs_process = true;
		}
	}
	if (needs_process && !has_ready) {
		out.append("\tvoid _ready() override;\n");
	}
	for (const GritGdxSkipped &entry : stub_methods(p_class)) {
		out.append("\tvoid _grit_unconverted_" + sanitize(String(entry.name)) + "(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error);\n");
	}

	if (has_coroutines) {
		// Each awaiting method gets a heap state object: every local and loop
		// counter lives there so the step method can resume mid-body.
		out.append("\nprivate:\n");
		for (const GritFunction &p_function : p_class.functions) {
			if (p_function.await_count == 0 || !emitter.is_supported(String(p_function.name))) {
				continue;
			}
			const String name = String(p_function.name);
			const String struct_name = await_struct_name(name);
			out.append("\tstruct " + struct_name + " {\n");
			out.append("\t\tint64_t ip = 0;\n");
			out.append("\t\tint64_t handle = 0;\n");
			out.append("\t\tVariant awaited;\n");
			const LocalVector<StateField> *fields = emitter.await_state_fields.getptr(p_function.name);
			if (fields) {
				for (const StateField &field : *fields) {
					out.append("\t\t" + field.type + " " + field.name + " = " + field.initializer + ";\n");
				}
			}
			out.append("\t};\n");
			out.append("\tVariant " + await_step_method(name) + "(" + struct_name + " *p_state);\n");
			out.append("\tvoid " + await_resume_method(name) + "(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error);\n");
			out.append("\tHashMap<int64_t, " + struct_name + " *> " + await_map_name(name) + ";\n\n");
		}
		out.append("\tint64_t grit_await_next = 0;\n");
	}

	out.append("};\n\n} // namespace godot\n");
	return out.as_string();
}

String GritGdxEmitter::emit_source(const GritGdxClass &p_class) {
	StringBuilder out;
	out.append("#include \"" + header_file_name(p_class) + "\"\n\n");
	out.append("#include <godot_cpp/core/class_db.hpp>\n");
	out.append("#include <godot_cpp/core/memory.hpp>\n");
	out.append("#include <godot_cpp/core/object.hpp>\n");
	out.append("#include <godot_cpp/variant/utility_functions.hpp>\n");
	out.append("#include <godot_cpp/templates/hash_map.hpp>\n");
	out.append("#include <godot_cpp/templates/pair.hpp>\n");
	// Singleton constants (Time, Engine, Input, …) are emitted as
	// <Class>::get_singleton(), which needs the class header.
	HashSet<String> singleton_includes;
	for (const GritFunction &function_entry : p_class.functions) {
		for (uint32_t i = 0; i < function_entry.constant_types.size(); i++) {
			if (function_entry.constant_types[i] != Variant::OBJECT || i >= function_entry.constant_values.size()) {
				continue;
			}
			const Object *object = function_entry.constant_values[i];
			if (!object) {
				continue;
			}
			const StringName class_name = object->get_class_name();
			if (ClassDB::class_exists(class_name) && Engine::get_singleton()->has_singleton(class_name)) {
				singleton_includes.insert(String(class_name));
			}
		}
	}
	for (const String &name : singleton_includes) {
		out.append("#include <godot_cpp/classes/" + snake_case(name) + ".hpp>\n");
	}
	out.append("\n");
	out.append("using namespace godot;\n\n");


	GritGdxEmitter emitter(p_class);
	emitter.prepare();

	for (const GritGdxProperty &property : p_class.properties) {
		const String type = cpp_storage(property.storage);
		out.append(type + " " + p_class.class_name + "::" + property.getter + "() const {\n");
		out.append("\treturn " + property.field + ";\n");
		out.append("}\n\n");
		out.append("void " + p_class.class_name + "::" + property.setter + "(" + (property.storage == Variant::NIL ? String("const Variant &p_value") : type + " p_value") + ") {\n");
		out.append("\t" + property.field + " = p_value;\n");
		out.append("}\n\n");
	}

	bool has_ready = emitter.is_supported("_ready");
	bool needs_process = false;
	bool has_coroutines = false;
	for (const GritFunction &p_function : p_class.functions) {
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		if (p_function.name == StringName("_process") || p_function.name == StringName("_physics_process")) {
			needs_process = true;
		}
		if (p_function.await_count > 0) {
			has_coroutines = true;
		}
	}

	// Methods the backend cannot translate exist as vararg stubs so a call fails
	// loudly instead of silently doing nothing.
	const LocalVector<GritGdxSkipped> stubs = stub_methods(p_class);

	// A node can be freed with awaits pending; those states hold Variants, so
	// they are released with the class instance.
	if (has_coroutines) {
		out.append(p_class.class_name + "::~" + p_class.class_name + "() {\n");
		for (const GritFunction &p_function : p_class.functions) {
			if (p_function.await_count == 0 || !emitter.is_supported(String(p_function.name))) {
				continue;
			}
			out.append("\tfor (const KeyValue<int64_t, " + await_struct_name(String(p_function.name)) + " *> &entry : " + await_map_name(String(p_function.name)) + ") {\n");
			out.append("\t\tmemdelete(entry.value);\n");
			out.append("\t}\n");
		}
		out.append("}\n\n");
	}

	for (const GritFunction &p_function : p_class.functions) {
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		const bool is_virtual = is_virtual_name(String(p_function.name));
		StringBuilder emitted;
		if (!emitter.emit_function(p_function, is_virtual, emitted)) {
			continue;
		}
		out.append(emitted);
	}

	if (needs_process && !has_ready) {
		out.append("void " + p_class.class_name + "::_ready() {\n");
		for (const GritFunction &p_function : p_class.functions) {
			if (p_function.name == StringName("_process")) {
				out.append("\tset_process(true);\n");
			}
			if (p_function.name == StringName("_physics_process")) {
				out.append("\tset_physics_process(true);\n");
			}
		}
		out.append("}\n\n");
	}

	for (const GritGdxSkipped &entry : stubs) {
		const String stub = "_grit_unconverted_" + sanitize(String(entry.name));
		out.append("void " + p_class.class_name + "::" + stub + "(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error) {\n");
		out.append("\tr_error.error = GDEXTENSION_CALL_OK;\n");
		out.append("\tUtilityFunctions::push_error(String(" + escaped_string("Grit: \"" + String(entry.name) + "\" was not converted (" + String(entry.reason) + ").") + "));\n");
		out.append("}\n\n");
	}

	out.append("void " + p_class.class_name + "::_bind_methods() {\n");
	for (const GritGdxProperty &property : p_class.properties) {
		out.append("\tClassDB::bind_method(D_METHOD(\"" + property.getter + "\"), &" + p_class.class_name + "::" + property.getter + ");\n");
		out.append("\tClassDB::bind_method(D_METHOD(\"" + property.setter + "\", \"value\"), &" + p_class.class_name + "::" + property.setter + ");\n");
	}
	for (const GritGdxSignal &signal : p_class.signals) {
		String arguments;
		for (uint32_t i = 0; i < signal.arguments.size(); i++) {
			if (i > 0) {
				arguments += ", ";
			}
			const PropertyInfo &info = signal.arguments[i];
			arguments += "PropertyInfo(Variant::" + variant_enum(info.type) + ", " + escaped_string(String(info.name)) + ")";
		}
		out.append("\tADD_SIGNAL(MethodInfo(" + escaped_string(String(signal.name)) + (arguments.is_empty() ? String() : ", " + arguments) + "));\n");
	}
	for (const GritFunction &p_function : p_class.functions) {
		if (is_virtual_name(String(p_function.name))) {
			continue;
		}
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		String names;
		int count = 0;
		for (const GritVariable &variable : p_function.variables) {
			if (variable.role != GritVariable::Role::PARAMETER) {
				continue;
			}
			if (count > 0) {
				names += ", ";
			}
			names += escaped_string(sanitize(variable.name));
			count++;
		}
		out.append("\tClassDB::bind_method(D_METHOD(" + escaped_string(sanitize(String(p_function.name))) + (names.is_empty() ? String() : ", " + names) + "), &" + p_class.class_name + "::" + sanitize(String(p_function.name)) + ");\n");
	}
	for (const GritGdxSkipped &entry : stubs) {
		const String stub = "_grit_unconverted_" + sanitize(String(entry.name));
		out.append("\t{\n");
		out.append("\t\tMethodInfo stub_info;\n");
		out.append("\t\tstub_info.name = " + escaped_string(String(entry.name)) + ";\n");
		out.append("\t\tClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, " + escaped_string(String(entry.name)) + ", &" + p_class.class_name + "::" + stub + ", stub_info);\n");
		out.append("\t}\n");
	}
	for (const GritFunction &p_function : p_class.functions) {
		if (p_function.await_count == 0 || !emitter.is_supported(String(p_function.name))) {
			continue;
		}
		// The awaited signal carries an arbitrary argument list, so the resume
		// entry point has to be a vararg method (bound handle + signal args).
		const String resume = await_resume_method(String(p_function.name));
		out.append("\t{\n");
		out.append("\t\tMethodInfo resume_info;\n");
		out.append("\t\tresume_info.name = " + escaped_string(resume) + ";\n");
		out.append("\t\tClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, " + escaped_string(resume) + ", &" + p_class.class_name + "::" + resume + ", resume_info);\n");
		out.append("\t}\n");
	}
	for (const GritGdxProperty &property : p_class.properties) {
		// Every script variable is a property so `object.name` keeps working;
		// only @export-ed ones are shown in the inspector.
		const PropertyInfo &info = property.info;
		const int usage = property.is_export ? int(info.usage & ~PROPERTY_USAGE_SCRIPT_VARIABLE) : int(PROPERTY_USAGE_STORAGE);
		out.append("\tADD_PROPERTY(PropertyInfo(Variant::" + variant_enum(property.is_export ? info.type : (info.type == Variant::NIL ? Variant::NIL : info.type)) + ", " + escaped_string(property.name) + ", static_cast<PropertyHint>(" + itos(int(info.hint)) + "), " + escaped_string(info.hint_string) + ", " + itos(usage) + "), " + escaped_string(property.setter) + ", " + escaped_string(property.getter) + ");\n");
	}
	out.append("}\n");
	return out.as_string();
}

LocalVector<GritGdxSkipped> GritGdxEmitter::stub_methods(const GritGdxClass &p_class) {
	LocalVector<GritGdxSkipped> stubs;
	for (const GritGdxSkipped &entry : skipped_functions(p_class)) {
		if (!is_virtual_name(String(entry.name))) {
			stubs.push_back(entry);
		}
	}
	// Virtuals are never stubbed: binding a name like _ready would shadow the
	// engine's own virtual call.
	for (const GritGdxSkipped &entry : p_class.skipped) {
		if (!String(entry.name).begins_with("@") && !is_virtual_name(String(entry.name))) {
			stubs.push_back(entry);
		}
	}
	return stubs;
}

LocalVector<GritGdxSkipped> GritGdxEmitter::skipped_functions(const GritGdxClass &p_class) {
	LocalVector<GritGdxSkipped> skipped;
	GritGdxEmitter emitter(p_class);
	emitter.prepare();
	for (const GritFunction &function_entry : p_class.functions) {
		if (emitter.is_supported(String(function_entry.name))) {
			continue;
		}
		emitter.reject_reason = String();
		StringBuilder probe;
		emitter.emit_function(function_entry, is_virtual_name(String(function_entry.name)), probe);
		GritGdxSkipped entry;
		entry.name = String(function_entry.name);
		entry.reason = emitter.reject_reason.is_empty() ? "not supported" : emitter.reject_reason;
		skipped.push_back(entry);
	}
	return skipped;
}


String GritGdxEmitter::helpers_file_name() {
	return "grit_gdx_helpers.h";
}

String GritGdxEmitter::emit_helpers(const GritGdxClass &p_class) {
	StringBuilder helpers;
	helpers.append("#pragma once\n\n");
	helpers.append("#include <godot_cpp/variant/variant.hpp>\n");
	helpers.append("#include <godot_cpp/variant/array.hpp>\n");
	helpers.append("#include <godot_cpp/variant/dictionary.hpp>\n");
	helpers.append("#include <godot_cpp/variant/string.hpp>\n");
	helpers.append("#include <godot_cpp/variant/signal.hpp>\n");
	helpers.append("#include <godot_cpp/variant/callable.hpp>\n");
	helpers.append("#include <godot_cpp/variant/callable_custom.hpp>\n");
	helpers.append("#include <godot_cpp/core/object_id.hpp>\n");
	helpers.append("#include <godot_cpp/variant/utility_functions.hpp>\n");
	helpers.append("#include <godot_cpp/core/object.hpp>\n\n");
	helpers.append("#include <initializer_list>\n\n");
	helpers.append("using namespace godot;\n\n");
	helpers.append("namespace grit_gdx {\n\n");
	helpers.append("inline Object *object_of(const Variant &p_value) {\n");
	helpers.append("\treturn static_cast<Object *>(p_value);\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant binary(Variant::Operator p_operator, const Variant &p_left, const Variant &p_right) {\n");
	helpers.append("\tVariant result;\n");
	helpers.append("\tbool valid = false;\n");
	helpers.append("\tVariant::evaluate(p_operator, p_left, p_right, result, valid);\n");
	helpers.append("\tif (!valid) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: incompatible operand types.\");\n");
	helpers.append("\t}\n");
	helpers.append("\treturn result;\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant get_named(const Variant &p_object, const StringName &p_name) {\n");
	helpers.append("\treturn object_of(p_object)->get(p_name);\n");
	helpers.append("}\n\n");
	// await: either wait on the signal carried by the operand, or take the
	// value and continue (GDScript returns non-signal operands immediately).
	// Returns 1 to suspend, 0 to continue, -1 after a reported error.
	helpers.append("inline int await_prepare(const Variant &p_operand, Signal &r_signal, Variant &r_value) {\n");
	helpers.append("\tVariant result = p_operand;\n");
	helpers.append("\tif (p_operand.get_type() == Variant::OBJECT) {\n");
	helpers.append("\t\tObject *object = p_operand;\n");
	helpers.append("\t\tif (object == nullptr) {\n");
	helpers.append("\t\t\tUtilityFunctions::push_error(\"Grit: trying to await on a freed object.\");\n");
	helpers.append("\t\t\treturn -1;\n");
	helpers.append("\t\t}\n");
	helpers.append("\t\tif (object->is_class(StringName(\"GDScriptFunctionState\"))) {\n");
	helpers.append("\t\t\t// A GDScript coroutine: wait for its completion signal.\n");
	helpers.append("\t\t\tresult = Variant(Signal(object, StringName(\"completed\")));\n");
	helpers.append("\t\t}\n");
	helpers.append("\t}\n");
	helpers.append("\tif (result.get_type() != Variant::SIGNAL) {\n");
	helpers.append("\t\tr_value = result;\n");
	helpers.append("\t\treturn 0;\n");
	helpers.append("\t}\n");
	helpers.append("\tr_signal = static_cast<Signal>(result);\n");
	helpers.append("\treturn 1;\n");
	helpers.append("}\n\n");
	helpers.append("inline bool is_native(const Variant &p_value, const StringName &p_class) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::OBJECT) {\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\tObject *object = p_value;\n");
	helpers.append("\tif (object == nullptr) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: the left operand of 'is' is a previously freed instance.\");\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\treturn object->is_class(p_class);\n");
	helpers.append("}\n\n");
	helpers.append("inline bool is_typed_array(const Variant &p_value, int64_t p_builtin_type, const StringName &p_native_type) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::ARRAY) {\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\tArray array = p_value;\n");
	helpers.append("\t// A typed array with a script element type reports it as an object; script\n");
	helpers.append("\t// element types are not converted, so only the engine's null counts.\n");
	helpers.append("\tObject *script = array.get_typed_script();\n");
	helpers.append("\treturn array.get_typed_builtin() == p_builtin_type && array.get_typed_class_name() == p_native_type && script == nullptr;\n");
	helpers.append("}\n\n");
	helpers.append("inline bool is_typed_dictionary(const Variant &p_value, int64_t p_key_builtin, const StringName &p_key_native, int64_t p_value_builtin, const StringName &p_value_native) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::DICTIONARY) {\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\tDictionary dictionary = p_value;\n");
	helpers.append("\tObject *key_script = dictionary.get_typed_key_script();\n");
	helpers.append("\tObject *value_script = dictionary.get_typed_value_script();\n");
	helpers.append("\treturn dictionary.get_typed_key_builtin() == p_key_builtin && dictionary.get_typed_key_class_name() == p_key_native && key_script == nullptr &&\n");
	helpers.append("\t\t\tdictionary.get_typed_value_builtin() == p_value_builtin && dictionary.get_typed_value_class_name() == p_value_native && value_script == nullptr;\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant cast_builtin(const Variant &p_value, int64_t p_type) {\n");
	helpers.append("\treturn UtilityFunctions::type_convert(p_value, p_type);\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant cast_native(const Variant &p_value, const StringName &p_class) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::OBJECT && p_value.get_type() != Variant::NIL) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: cannot convert a non-object value to an object type.\");\n");
	helpers.append("\t\treturn Variant();\n");
	helpers.append("\t}\n");
	helpers.append("\tObject *object = p_value;\n");
	helpers.append("\tif (object == nullptr) {\n");
	helpers.append("\t\treturn Variant();\n");
	helpers.append("\t}\n");
	helpers.append("\treturn object->is_class(p_class) ? p_value : Variant();\n");
	helpers.append("}\n\n");
	helpers.append("inline void set_named(const Variant &p_object, const StringName &p_name, const Variant &p_value) {\n");
	helpers.append("\tobject_of(p_object)->set(p_name, p_value);\n");
helpers.append("}\n\n");
	helpers.append("inline Variant get_keyed(const Variant &p_container, const Variant &p_key) {\n");
	helpers.append("\tif (p_container.get_type() == Variant::DICTIONARY) {\n");
	helpers.append("\t\tDictionary dictionary = p_container;\n");
	helpers.append("\t\treturn dictionary[p_key];\n");
	helpers.append("\t}\n");
	helpers.append("\tif (p_container.get_type() == Variant::STRING) {\n");
	helpers.append("\t\tString text = p_container;\n");
	helpers.append("\t\treturn text[static_cast<int64_t>(p_key)];\n");
	helpers.append("\t}\n");
	helpers.append("\tArray array = p_container;\n");
	helpers.append("\treturn array[static_cast<int64_t>(p_key)];\n");
	helpers.append("}\n\n");
	helpers.append("inline void set_keyed(const Variant &p_container, const Variant &p_key, const Variant &p_value) {\n");
	helpers.append("\tif (p_container.get_type() == Variant::DICTIONARY) {\n");
	helpers.append("\t\tDictionary dictionary = p_container;\n");
	helpers.append("\t\tdictionary[p_key] = p_value;\n");
	helpers.append("\t\treturn;\n");
	helpers.append("\t}\n");
	helpers.append("\tArray array = p_container;\n");
	helpers.append("\tarray[static_cast<int64_t>(p_key)] = p_value;\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant call_method(Variant p_base, const StringName &p_method) {\n");
	helpers.append("\treturn p_base.call(p_method);\n");
	helpers.append("}\n\n");
	helpers.append("template <typename... Args>\n");
	helpers.append("inline Variant call_method(Variant p_base, const StringName &p_method, const Args &...p_args) {\n");
	helpers.append("\treturn p_base.call(p_method, p_args...);\n");
	helpers.append("}\n\n");
	helpers.append("inline Array iterate(const Variant &p_container) {\n");
	helpers.append("\tif (p_container.get_type() == Variant::DICTIONARY) {\n");
	helpers.append("\t\tDictionary dictionary = p_container;\n");
	helpers.append("\t\treturn dictionary.keys();\n");
	helpers.append("\t}\n");
	helpers.append("\treturn static_cast<Array>(p_container);\n");
	helpers.append("}\n\n");
	helpers.append("inline Array array(std::initializer_list<Variant> p_items) {\n");
	helpers.append("\tArray result;\n");
	helpers.append("\tfor (const Variant &item : p_items) {\n");
	helpers.append("\t\tresult.push_back(item);\n");
	helpers.append("\t}\n");
	helpers.append("\treturn result;\n");
	helpers.append("}\n\n");
	helpers.append("inline Dictionary dictionary(std::initializer_list<Variant> p_items) {\n");
	helpers.append("\tDictionary result;\n");
	helpers.append("\tauto item = p_items.begin();\n");
	helpers.append("\twhile (item != p_items.end()) {\n");
	helpers.append("\t\tconst Variant &key = *item++;\n");
	helpers.append("\t\tresult[key] = item != p_items.end() ? *item++ : Variant();\n");
	helpers.append("\t}\n");
	helpers.append("\treturn result;\n");
	helpers.append("}\n\n");
	// Godot compares connected callables by their base method, ignoring bound
	// arguments, so a single bound resume method could only ever carry one
	// pending await. Every suspension therefore connects an instance of this
	// callable: distinct instances never compare equal, and get_object() ties
	// the connection to the node so the engine drops it if the node dies.
	helpers.append("class AwaitCallback : public CallableCustom {\n");
	helpers.append("public:\n");
	helpers.append("\tAwaitCallback(Object *p_owner, int64_t p_handle, const StringName &p_method) :\n");
	helpers.append("\t\t\towner(p_owner), owner_id(p_owner->get_instance_id()), handle(p_handle), method(p_method) {}\n");
	helpers.append("\n");
	helpers.append("\tuint32_t hash() const override {\n");
	helpers.append("\t\treturn static_cast<uint32_t>(static_cast<uint64_t>(owner_id)) ^ static_cast<uint32_t>(handle);\n");
	helpers.append("\t}\n");
	helpers.append("\tString get_as_text() const override {\n");
	helpers.append("\t\treturn \"Grit await\";\n");
	helpers.append("\t}\n");
	helpers.append("\tstatic bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {\n");
	helpers.append("\t\treturn p_a == p_b;\n");
	helpers.append("\t}\n");
	helpers.append("\tstatic bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {\n");
	helpers.append("\t\treturn p_a < p_b;\n");
	helpers.append("\t}\n");
	helpers.append("\tCompareEqualFunc get_compare_equal_func() const override {\n");
	helpers.append("\t\treturn &compare_equal;\n");
	helpers.append("\t}\n");
	helpers.append("\tCompareLessFunc get_compare_less_func() const override {\n");
	helpers.append("\t\treturn &compare_less;\n");
	helpers.append("\t}\n");
	helpers.append("\tObjectID get_object() const override {\n");
	helpers.append("\t\treturn owner_id;\n");
	helpers.append("\t}\n");
	helpers.append("\tvoid call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const override {\n");
	helpers.append("\t\tArray arguments;\n");
	helpers.append("\t\tfor (int i = 0; i < p_argcount; i++) {\n");
	helpers.append("\t\t\targuments.push_back(*p_arguments[i]);\n");
	helpers.append("\t\t}\n");
	helpers.append("\t\targuments.push_back(Variant(handle));\n");
	helpers.append("\t\towner->callv(method, arguments);\n");
	helpers.append("\t\tr_return_value = Variant();\n");
	helpers.append("\t\tr_call_error.error = GDEXTENSION_CALL_OK;\n");
	helpers.append("\t}\n");
	helpers.append("\n");
	helpers.append("private:\n");
	helpers.append("\tObject *owner;\n");
	helpers.append("\tObjectID owner_id;\n");
	helpers.append("\tint64_t handle;\n");
	helpers.append("\tStringName method;\n");
	helpers.append("};\n\n");
	helpers.append("} // namespace grit_gdx\n\n");
	return helpers.as_string();
}
