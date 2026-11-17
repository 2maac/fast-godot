#include "grit_gdx_emitter.h"

#include "core/math/math_funcs.h"
#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
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
	prepare_lambdas();
}

bool GritGdxEmitter::is_lambda_function(const GritFunction &p_function) {
	return p_function.registry_key.contains(">");
}

// "res://x.gd::f:3><anonymous lambda>:12#0" -> 12
int GritGdxEmitter::lambda_key_line(const String &p_key) {
	const int hash = p_key.rfind("#");
	if (hash < 0) {
		return -1;
	}
	const int colon = p_key.rfind(":", hash);
	if (colon < 0) {
		return -1;
	}
	return p_key.substr(colon + 1, hash - colon - 1).to_int();
}

const GritGdxEmitter::LambdaInfo *GritGdxEmitter::lambda_info(const GritFunction &p_function) const {
	return lambdas_by_key.getptr(p_function.registry_key);
}

// Assigns a method name to every lambda and pairs each creation site with the
// lambda function the front end recorded for it (matched by line).
void GritGdxEmitter::prepare_lambdas() {
	lambdas_by_key.clear();
	lambda_sites.clear();
	LocalVector<const GritFunction *> lambdas;
	for (const GritFunction &function_entry : klass.functions) {
		if (!is_lambda_function(function_entry)) {
			continue;
		}
		LambdaInfo info;
		info.method = "_grit_lambda_" + itos(lambdas.size());
		info.kind = int(lambdas.size());
		lambdas_by_key.insert(function_entry.registry_key, info);
		lambdas.push_back(&function_entry);
	}
	const bool debug_lambdas = OS::get_singleton()->get_environment("GRIT_GDX_DEBUG_LAMBDAS") == "1";
	if (debug_lambdas) {
		print_line(vformat("Grit debug: %d lambda function(s), %d class function(s)", lambdas.size(), klass.functions.size()));
		for (const GritFunction *lambda : lambdas) {
			print_line("  lambda key: " + lambda->registry_key);
		}
		for (const GritFunction &f : klass.functions) {
			if (!is_lambda_function(f)) {
				print_line("  function key: " + f.registry_key);
			}
		}
	}
	if (lambdas.is_empty()) {
		return;
	}
	for (const GritFunction &function_entry : klass.functions) {
		if (is_lambda_function(function_entry)) {
			continue;
		}
		LocalVector<const GritFunction *> candidates;
		for (const GritFunction *lambda : lambdas) {
			if (lambda->registry_key.begins_with(String(function_entry.registry_key) + ">")) {
				candidates.push_back(lambda);
			}
		}
		// LocalVector<bool>::resize does not initialise trivial elements; leaving
		// them uninitialised made the pairing depend on heap garbage.
		LocalVector<uint8_t> used;
		used.resize(candidates.size());
		for (uint32_t i = 0; i < used.size(); i++) {
			used[i] = 0;
		}
		const auto take = [&](int p_line) -> int {
			for (uint32_t i = 0; i < candidates.size(); i++) {
				if (!used[i] && lambda_key_line(candidates[i]->registry_key) == p_line) {
					used[i] = 1;
					return int(i);
				}
			}
			for (uint32_t i = 0; i < candidates.size(); i++) {
				if (!used[i]) {
					used[i] = 1;
					return int(i);
				}
			}
			return -1;
		};
		for (const GritBlock &block : function_entry.blocks) {
			for (const GritStatement &statement : block.statements) {
				if (statement.opcode != GritOpcode::CREATE_LAMBDA) {
					continue;
				}
				const int match = take(statement.line);
				if (debug_lambdas) {
					print_line(vformat("  site line %d -> match %d (%s)", statement.line, match, match >= 0 ? candidates[match]->registry_key : String("none")));
				}
				if (match < 0) {
					continue;
				}
				const GritFunction *lambda = candidates[match];
				const LambdaInfo *info = lambdas_by_key.getptr(lambda->registry_key);
				LambdaSite site;
				site.method = info->method;
				site.key = lambda->registry_key;
				site.kind = info->kind;
				site.use_self = statement.validated;
				site.captures = int(statement.operands.size());
				site.declared = lambda->parameter_count - site.captures;
				lambda_sites.insert(&statement, site);
				LambdaInfo updated = *info;
				updated.use_self = statement.validated;
				lambdas_by_key.insert(lambda->registry_key, updated);
			}
		}
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
	supported_lambdas.clear();
	binding_defaults_by_function.clear();
	declaration_parameters_by_function.clear();
	binding_static_by_function.clear();

	// Lambdas first: a creation site needs to know whether its lambda converts
	// before the enclosing method can be emitted.
	for (const GritFunction &function_entry : klass.functions) {
		if (!is_lambda_function(function_entry)) {
			continue;
		}
		StringBuilder probe;
		if (emit_function(function_entry, false, probe)) {
			supported_lambdas.insert(function_entry.registry_key);
		} else if (OS::get_singleton()->get_environment("GRIT_GDX_DEBUG_LAMBDAS") == "1") {
			print_line("  lambda failed: " + function_entry.registry_key + " -> " + (reject_reason.is_empty() ? String("no reason") : reject_reason));
		}
	}

	// A call site asks whether the callee converts, and the callee may appear
	// later in the recording, so support is resolved to a fixpoint: every pass
	// may mark more methods as convertible and none is unmarked.
	for (int pass = 0; pass < 4; pass++) {
		bool grew = false;
		for (const GritFunction &function_entry : klass.functions) {
			if (is_lambda_function(function_entry)) {
				continue;
			}
			const bool was = is_supported(String(function_entry.name));
			StringBuilder probe;
			const bool is_virtual = is_virtual_name(String(function_entry.name));
			const bool now = emit_function(function_entry, is_virtual, probe);
			if (now && !was) {
				grew = true;
			}
			if (now) {
				supported_methods.insert(function_entry.name, true);
			} else if (!was) {
				supported_methods.insert(function_entry.name, false);
			}
		}
		if (!grew) {
			break;
		}
	}

	// The metadata (defaults, declarations, static-ness) is recorded by a last
	// pass over the settled support set.
	binding_defaults_by_function.clear();
	declaration_parameters_by_function.clear();
	binding_static_by_function.clear();
	for (const GritFunction &function_entry : klass.functions) {
		const bool lambda = is_lambda_function(function_entry);
		if (lambda ? !supported_lambdas.has(function_entry.registry_key) : !is_supported(String(function_entry.name))) {
			continue;
		}
		StringBuilder probe;
		const bool is_virtual = !lambda && is_virtual_name(String(function_entry.name));
		emit_function(function_entry, is_virtual, probe);
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

String GritGdxEmitter::variable_name(int p_variable) const {
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
			if (static_function) {
				reject("self in a static function");
				return "Variant()";
			}
			return "this";
		case GritOperand::Kind::CONSTANT:
			return constant_expression(p_operand.index);
		case GritOperand::Kind::MEMBER: {
			if (static_function) {
				reject("member access in a static function");
				return "Variant()";
			}
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
	if (return_type.kind == GritType::Kind::BUILTIN && return_type.builtin_type == Variant::NIL) {
		line("return;");
		return true;
	}
	if (return_type.kind == GritType::Kind::VARIANT) {
		line("return " + read_any(p_statement.operands[0]) + ";");
		return true;
	}
	if (return_type.kind == GritType::Kind::BUILTIN) {
		const String typed = typed_container_expression(return_type, read_any(p_statement.operands[0]));
		line("return " + (typed.is_empty() ? read_as(p_statement.operands[0], return_type.builtin_type) : typed) + ";");
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
			// GDScript compiles a call to a static method of this class with a CLASS
			// base, and to an instance method with SELF.
			const bool callee_is_static = method && String(method->signature).begins_with("static ");
			const bool self_call = method && is_supported(name) && (base.kind == GritOperand::Kind::SELF || (base.kind == GritOperand::Kind::CLASS && callee_is_static));
			String expression;
			if (self_call) {
				expression = String(callee_is_static ? klass.class_name + "::" : "this->") + sanitize(name) + "(" + call_arguments(p_statement, 1, method) + ")";
			} else if (base.kind == GritOperand::Kind::CLASS) {
				reject("a class-level call that is not a static method of this class");
				return false;
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
				// A void method has no value to hand on, so the result slot is
				// cleared instead of assigned.
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
			line("const int64_t " + from_symbol + " = " + from + ";");
			line("const int64_t " + to_symbol + " = " + to + ";");
			line("const int64_t " + step_symbol + " = " + step + ";");
			line("for (int64_t " + counter + " = " + from_symbol + "; " + step_symbol + " != 0 && (" + step_symbol + " > 0 ? " + counter + " < " + to_symbol + " : " + counter + " > " + to_symbol + "); " + counter + " += " + step_symbol + ") {");
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
			line("const " + container_type + " " + container_symbol + " = " + container_value + ";");
			// An int container is its own upper bound; arrays, strings and
			// dictionary keys all expose a size.
			String size_expression = container_symbol;
			if (container_storage == Variant::STRING) {
				size_expression = container_symbol + ".length()";
			} else if (container_storage != Variant::INT) {
				size_expression = container_symbol + ".size()";
			}
			line("for (int64_t " + index_symbol + " = 0; " + index_symbol + " < " + size_expression + "; " + index_symbol + "++) {");
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
// (or a returned void) does.
void GritGdxEmitter::write_abort_return() {
	const String return_cpp = return_type_of(*function);
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

// A GDScript default parameter value is a constant; the front end records it as
// an assignment inside the DEFAULT_ARGUMENT block of that parameter.
bool GritGdxEmitter::optional_parameter_default(const GritFunction &p_function, int p_parameter_variable, String &r_expression) {
	for (const GritStatement &statement : p_function.blocks[0].statements) {
		if (statement.opcode != GritOpcode::DEFAULT_ARGUMENT || statement.index != p_parameter_variable) {
			continue;
		}
		if (statement.blocks[0] >= 0 && statement.blocks[0] < int(p_function.blocks.size())) {
			for (const GritStatement &inner : p_function.blocks[statement.blocks[0]].statements) {
				if ((inner.opcode == GritOpcode::ASSIGN || inner.opcode == GritOpcode::ASSIGN_TYPED) && inner.target.kind == GritOperand::Kind::VARIABLE && inner.target.index == p_parameter_variable && !inner.operands.is_empty()) {
					if (inner.operands[0].kind != GritOperand::Kind::CONSTANT) {
						reject("optional parameter default is not a constant");
						return false;
					}
					r_expression = constant_expression(inner.operands[0].index);
					return true;
				}
			}
		}
		reject("optional parameter without a recorded default");
		return false;
	}
	reject("optional parameter without a recorded default");
	return false;
}

// The declared type of an operand, including container element types, which the
// storage-only view drops.
GritType GritGdxEmitter::declared_type_of(const GritOperand &p_operand) const {
	switch (p_operand.kind) {
		case GritOperand::Kind::VARIABLE:
			if (p_operand.index >= 0 && p_operand.index < int(function->variables.size())) {
				return function->types[function->variables[p_operand.index].type];
			}
			break;
		case GritOperand::Kind::MEMBER: {
			const GritGdxProperty *property = property_of(p_operand.index);
			if (property) {
				return GritType::from(property->type);
			}
		} break;
		default:
			break;
	}
	return GritType();
}

bool GritGdxEmitter::container_element_arguments(const GritType &p_element, String &r_builtin, String &r_native) {
	if (p_element.kind == GritType::Kind::SCRIPT) {
		reject("script-typed container elements");
		return false;
	}
	const Variant::Type builtin = p_element.kind == GritType::Kind::NATIVE ? Variant::OBJECT : p_element.builtin_type;
	r_builtin = "Variant::" + variant_enum(builtin);
	r_native = "StringName(" + escaped_string(p_element.kind == GritType::Kind::NATIVE ? String(p_element.native_type) : String()) + ")";
	return true;
}

// `grit_gdx::typed_array(<value>, …)` when the declared type is a typed
// container, empty otherwise (the caller keeps its plain assignment).
String GritGdxEmitter::typed_container_expression(const GritType &p_type, const String &p_value) {
	if (p_type.kind != GritType::Kind::BUILTIN || !p_type.has_elements()) {
		return String();
	}
	if (p_type.builtin_type == Variant::ARRAY) {
		String builtin;
		String native;
		if (!container_element_arguments(p_type.elements[0], builtin, native)) {
			return String();
		}
		return "grit_gdx::typed_array(" + p_value + ", " + builtin + ", " + native + ")";
	}
	if (p_type.builtin_type == Variant::DICTIONARY) {
		String key_builtin;
		String key_native;
		String value_builtin;
		String value_native;
		if (!container_element_arguments(p_type.elements[0], key_builtin, key_native) || !container_element_arguments(p_type.elements[1], value_builtin, value_native)) {
			return String();
		}
		return "grit_gdx::typed_dictionary(" + p_value + ", " + key_builtin + ", " + key_native + ", " + value_builtin + ", " + value_native + ")";
	}
	return String();
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
			// Only a file-level script can be named precisely: an inner class path
			// ("res://x.gd::Inner") cannot be compared with an object's script.
			if (type.script.is_empty() || type.script.contains("::")) {
				reject("'is' with an inner-class script type");
				return false;
			}
			expression = "grit_gdx::is_script(" + value + ", String(" + escaped_string(type.script) + "))";
			break;
		case GritType::Kind::VARIANT:
			expression = "false";
			break;
	}
	line(lvalue(p_statement.target) + " = " + convert(expression, Variant::BOOL, storage_of(p_statement.target)) + ";");
	return true;
}

bool GritGdxEmitter::container_type_test(const GritType &p_type, const String &p_value, String &r_expression) {
	if (p_type.builtin_type == Variant::ARRAY) {
		String builtin;
		String native;
		if (!container_element_arguments(p_type.elements[0], builtin, native)) {
			return false;
		}
		r_expression = "grit_gdx::is_typed_array(" + p_value + ", " + builtin + ", " + native + ")";
		return true;
	}
	String key_builtin;
	String key_native;
	String value_builtin;
	String value_native;
	if (!container_element_arguments(p_type.elements[0], key_builtin, key_native) || !container_element_arguments(p_type.elements[1], value_builtin, value_native)) {
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
			if (type.script.is_empty() || type.script.contains("::")) {
				reject("'as' with an inner-class script type");
				return false;
			}
			expression = "grit_gdx::cast_script(" + value + ", String(" + escaped_string(type.script) + "))";
			break;
		case GritType::Kind::VARIANT:
			expression = value;
			break;
	}
	line(lvalue(p_statement.target) + " = " + convert(expression, Variant::NIL, storage_of(p_statement.target)) + ";");
	return true;
}

// `func(...): ...` becomes a CallableCustom that calls the lambda's internal
// method with the captured values prepended to the call arguments.
bool GritGdxEmitter::write_create_lambda(const GritStatement &p_statement) {
	const LambdaSite *site = lambda_sites.getptr(&p_statement);
	if (!site) {
		reject("a lambda creation site without a recorded function");
		return false;
	}
	if (!supported_lambdas.has(site->key)) {
		reject("a lambda in this method cannot be converted");
		return false;
	}
	String captures;
	for (uint32_t i = 0; i < p_statement.operands.size(); i++) {
		if (i > 0) {
			captures += ", ";
		}
		captures += read_any(p_statement.operands[i]);
	}
	const String capture_value = captures.is_empty() ? String("Array()") : "grit_gdx::array({ " + captures + " })";
	const String lambda_type = "GritLambda" + klass.class_name;
	line(lvalue(p_statement.target) + " = Callable(memnew(" + lambda_type + "(" + itos(site->kind) + ", " + capture_value + ", " + (site->use_self ? String("this") : String("nullptr")) + ")));");
	return true;
}

// One CallableCustom for every lambda of the class; `kind` selects which
// internal method receives the captures plus the call arguments.
void GritGdxEmitter::emit_lambda_callable(const GritGdxClass &p_class, const GritGdxEmitter &p_emitter, StringBuilder &r_out) {
	bool any = false;
	for (const GritFunction &function_entry : p_class.functions) {
		if (is_lambda_function(function_entry) && p_emitter.supported_lambdas.has(function_entry.registry_key)) {
			any = true;
			break;
		}
	}
	if (!any) {
		return;
	}
	const String type = "GritLambda" + p_class.class_name;
	r_out.append("namespace {\n\n");
	r_out.append("struct " + type + " : public CallableCustom {\n");
	r_out.append("\tint64_t kind = 0;\n");
	r_out.append("\tArray captures;\n");
	r_out.append("\tuint64_t owner_id = 0;\n");
	r_out.append("\t" + p_class.class_name + " *owner = nullptr;\n");
	r_out.append("\t" + type + "(int64_t p_kind, const Array &p_captures, " + p_class.class_name + " *p_owner) :\n");
	r_out.append("\t\t\tkind(p_kind), captures(p_captures), owner(p_owner), owner_id(p_owner ? p_owner->get_instance_id() : 0) {}\n");
	r_out.append("\tuint32_t hash() const override {\n\t\treturn static_cast<uint32_t>(owner_id) ^ static_cast<uint32_t>(kind);\n\t}\n");
	r_out.append("\tString get_as_text() const override {\n\t\treturn \"Grit lambda\";\n\t}\n");
	r_out.append("\tstatic bool compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {\n\t\treturn p_a == p_b;\n\t}\n");
	r_out.append("\tstatic bool compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {\n\t\treturn p_a < p_b;\n\t}\n");
	r_out.append("\tCompareEqualFunc get_compare_equal_func() const override {\n\t\treturn &compare_equal;\n\t}\n");
	r_out.append("\tCompareLessFunc get_compare_less_func() const override {\n\t\treturn &compare_less;\n\t}\n");
	r_out.append("\tObjectID get_object() const override {\n\t\treturn ObjectID(owner_id);\n\t}\n");
	// A plain lambda has no object, which the default is_valid() would reject.
	r_out.append("\tbool is_valid() const override {\n");
	r_out.append("\t\tif (owner_id == 0) {\n\t\t\treturn true;\n\t\t}\n");
	r_out.append("\t\treturn ObjectDB::get_instance(ObjectID(owner_id)) != nullptr;\n");
	r_out.append("\t}\n");
	r_out.append("\tvoid call(const Variant **p_args, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_error) const override {\n");
	r_out.append("\t\tr_error.error = GDEXTENSION_CALL_OK;\n");
	r_out.append("\t\tswitch (kind) {\n");
	for (const GritFunction &function_entry : p_class.functions) {
		if (!is_lambda_function(function_entry) || !p_emitter.supported_lambdas.has(function_entry.registry_key)) {
			continue;
		}
		const LambdaInfo *info = p_emitter.lambda_info(function_entry);
		const LambdaSite *site = nullptr;
		for (const KeyValue<const GritStatement *, LambdaSite> &entry : p_emitter.lambda_sites) {
			if (entry.value.key == function_entry.registry_key) {
				site = &entry.value;
				break;
			}
		}
		if (!info || !site) {
			continue;
		}
		LocalVector<Variant::Type> storages;
		for (const GritVariable &variable : function_entry.variables) {
			if (variable.role == GritVariable::Role::PARAMETER) {
				storages.push_back(variable.storage);
			}
		}
		String arguments;
		for (uint32_t i = 0; i < storages.size(); i++) {
			if (i > 0) {
				arguments += ", ";
			}
			const String source = int(i) < site->captures ? "captures[" + itos(i) + "]" : "*p_args[" + itos(int(i) - site->captures) + "]";
			arguments += p_emitter.convert(source, Variant::NIL, storages[i]);
		}
		const String call_expression = String(site->use_self ? "owner->" : p_class.class_name + "::") + info->method + "(" + arguments + ")";
		const String return_cpp = function_return_cpp(function_entry);
		r_out.append("\t\t\tcase " + itos(info->kind) + ": {\n");
		r_out.append("\t\t\t\tif (p_argcount != " + itos(site->declared) + ") {\n\t\t\t\t\tr_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;\n\t\t\t\t\treturn;\n\t\t\t\t}\n");
		if (site->use_self) {
			r_out.append("\t\t\t\tif (owner == nullptr) {\n\t\t\t\t\tr_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;\n\t\t\t\t\treturn;\n\t\t\t\t}\n");
		}
		if (return_cpp == "void") {
			r_out.append("\t\t\t\t" + call_expression + ";\n\t\t\t\tr_return_value = Variant();\n");
		} else {
			r_out.append("\t\t\t\tr_return_value = Variant(" + call_expression + ");\n");
		}
		r_out.append("\t\t\t\treturn;\n\t\t\t}\n");
	}
	r_out.append("\t\t\tdefault:\n\t\t\t\tr_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;\n\t\t\t\treturn;\n\t\t}\n");
	r_out.append("\t}\n};\n\n} // namespace\n\n");
}

bool GritGdxEmitter::write_statement(const GritStatement &p_statement) {
	last_opcode = int(p_statement.opcode);
	last_statement_line = p_statement.line;
	switch (p_statement.opcode) {
		case GritOpcode::ASSIGN:
		case GritOpcode::ASSIGN_TYPED: {
			const Variant::Type storage = storage_of(p_statement.target);
			// A typed container target is (re)built as one, so elements are
			// converted and the value keeps its element type.
			const String typed = typed_container_expression(declared_type_of(p_statement.target), read_any(p_statement.operands[0]));
			if (!typed.is_empty()) {
				line(lvalue(p_statement.target) + " = " + typed + ";");
			} else if (storage == Variant::OBJECT) {
				line(lvalue(p_statement.target) + " = " + convert(read_any(p_statement.operands[0]), Variant::NIL, Variant::OBJECT) + ";");
			} else {
				line(lvalue(p_statement.target) + " = " + read_as(p_statement.operands[0], storage) + ";");
			}
			return true;
		}
		case GritOpcode::CLEAR: {
			const String typed = typed_container_expression(declared_type_of(p_statement.target), "Variant()");
			line(lvalue(p_statement.target) + " = " + (typed.is_empty() ? default_storage_value(storage_of(p_statement.target)) : typed) + ";");
			return true;
		}
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
			return write_call(p_statement);
		case GritOpcode::CREATE_LAMBDA:
			return write_create_lambda(p_statement);
		case GritOpcode::AWAIT:
			reject("await (a GDExtension method cannot suspend and resume)");
			return false;
		case GritOpcode::TYPE_TEST:
			return write_type_test(p_statement);
		case GritOpcode::CAST:
			return write_cast(p_statement);
		case GritOpcode::GET_STATIC:
		case GritOpcode::SET_STATIC:
		case GritOpcode::GET_GLOBAL:
		case GritOpcode::DEFAULT_ARGUMENT:
			// Optional parameter defaults become C++ default arguments.
			return true;
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

// What the *entry* method returns: the engine's virtual signature or the script
// method's return type.
String GritGdxEmitter::entry_return_cpp(const GritFunction &p_function) {
	const VirtualInfo *info = is_virtual_name(String(p_function.name)) ? find_virtual(String(p_function.name)) : nullptr;
	if (info) {
		return String(info->return_type);
	}
	return function_return_cpp(p_function);
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

	lambda_function = is_lambda_function(p_function);
	const LambdaInfo *lambda = lambda_function ? lambda_info(p_function) : nullptr;
	if (lambda_function && !lambda) {
		reject("a lambda without a recorded creation site");
		return false;
	}
	static_function = lambda_function ? !lambda->use_self : String(p_function.signature).begins_with("static ");
	if (lambda_function && p_function.optional_parameter_count > 0) {
		reject("a lambda with optional parameters");
		return false;
	}
	if (lambda_function && p_function.rest_variable >= 0) {
		reject("a lambda with rest parameters");
		return false;
	}
	if (static_function && p_is_virtual) {
		reject("a static function cannot override a virtual method");
		return false;
	}
	if (p_function.await_count > 0) {
		reject("await (a GDExtension method cannot suspend and resume)");
		return false;
	}
	if (p_is_virtual && p_function.optional_parameter_count > 0) {
		reject("a virtual method cannot have optional parameters");
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
	String declaration_parameters;
	LocalVector<String> aliases;
	LocalVector<String> binding_defaults;
	if (virtual_info) {
		for (int i = 0; i < virtual_info->param_count; i++) {
			if (i > 0) {
				signature_parameters += ", ";
			}
			signature_parameters += String(virtual_info->params[i].type) + " " + String(virtual_info->params[i].name);
			const GritVariable &variable = p_function.variables[parameter_variables[i]];
			const String reference = variable_name(parameter_variables[i]);
			const String typed = typed_container_expression(function->types[variable.type], "Variant(" + String(virtual_info->params[i].name) + ")");
			aliases.push_back(cpp_storage(variable.storage) + " " + reference + " = " + (typed.is_empty() ? String(virtual_info->params[i].name) : typed) + ";");
		}
	} else {
		const int required = lambda_function ? p_function.parameter_count : p_function.parameter_count - p_function.optional_parameter_count;
		for (uint32_t i = 0; i < parameter_variables.size(); i++) {
			if (i > 0) {
				signature_parameters += ", ";
			}
			const GritVariable &variable = p_function.variables[parameter_variables[i]];
			const String parameter_name = "p_" + sanitize(variable.name);
			if (i > 0) {
				declaration_parameters += ", ";
			}
			declaration_parameters += function_parameter_cpp(variable, parameter_name);
			if (int(i) >= required) {
				String default_expression;
				if (!optional_parameter_default(p_function, parameter_variables[i], default_expression)) {
					return false;
				}
				declaration_parameters += " = " + convert(default_expression, Variant::NIL, variable.storage);
				binding_defaults.push_back("Variant(" + default_expression + ")");
			}
			signature_parameters += function_parameter_cpp(variable, parameter_name);
			const String reference = variable_name(parameter_variables[i]);
			const String typed = typed_container_expression(function->types[variable.type], "Variant(" + parameter_name + ")");
			aliases.push_back(cpp_storage(variable.storage) + " " + reference + " = " + (typed.is_empty() ? parameter_name : typed) + ";");
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
		if (reject_reason.is_empty()) {
			// Never report a bare "not supported": name the statement that failed.
			reject(vformat("could not convert a statement (opcode %d, line %d)", last_opcode, last_statement_line));
		}
		return false;
	}

	const String return_cpp = entry_return_cpp(p_function);
	const String method_name = lambda_function ? lambda->method : sanitize(String(p_function.name));
	const String qualified_name = klass.class_name + "::" + method_name;
	// `static` belongs to the declaration only.
	r_output.append(return_cpp + " " + qualified_name + "(" + signature_parameters + ") {\n");
	for (const String &alias : aliases) {
		r_output.append("\t" + alias + "\n");
	}
	for (uint32_t i = 0; i < p_function.variables.size(); i++) {
		const GritVariable &variable = p_function.variables[i];
		if (variable.role == GritVariable::Role::PARAMETER) {
			continue;
		}
		const String typed = typed_container_expression(p_function.types[variable.type], "Variant()");
		r_output.append("\t" + cpp_storage(variable.storage) + " " + variable_name(i) + " = " + (typed.is_empty() ? default_storage_value(variable.storage) : typed) + ";\n");
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

	binding_defaults_by_function.insert(p_function.registry_key, binding_defaults);
	declaration_parameters_by_function.insert(p_function.registry_key, declaration_parameters);
	binding_static_by_function.insert(p_function.registry_key, static_function);
	static_function = false;
	lambda_function = false;
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
	out.append("#include <godot_cpp/classes/ref.hpp>\n");
	out.append("#include <godot_cpp/classes/ref_counted.hpp>\n");
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
		const String typed = emitter.typed_container_expression(GritType::from(property.type), "Variant(" + initializer + ")");
		if (!typed.is_empty()) {
			initializer = typed;
		}
		out.append("\t" + cpp_storage(property.storage) + " " + property.field + " = " + initializer + ";\n");
	}

	out.append("\npublic:\n\tstatic void _bind_methods();\n\n");
	for (const GritGdxProperty &property : p_class.properties) {
		const String type = cpp_storage(property.storage);
		out.append("\t" + type + " " + property.getter + "() const;\n");
		out.append("\tvoid " + property.setter + "(" + (property.storage == Variant::NIL ? String("const Variant &p_value") : type + " p_value") + ");\n");
	}
	out.append("\n");

	bool has_ready = emitter.is_supported("_ready");
	for (const GritFunction &p_function : p_class.functions) {
		if (is_lambda_function(p_function)) {
			continue;
		}
		const bool is_virtual = is_virtual_name(String(p_function.name));
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		if (is_virtual) {
			out.append("\t" + virtual_signature_of(p_function) + ";\n");
			continue;
		}
		String parameters;
		if (const String *declaration = emitter.declaration_parameters_by_function.getptr(p_function.registry_key)) {
			parameters = *declaration;
		} else {
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
		}
		const bool is_static = String(p_function.signature).begins_with("static ");
		out.append("\t" + String(is_static ? "static " : "") + entry_return_cpp(p_function) + " " + sanitize(String(p_function.name)) + "(" + parameters + ");\n");
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
	// Lambdas are internal methods: a CallableCustom calls them with the
	// captured values prepended to the call arguments.
	for (const GritFunction &p_function : p_class.functions) {
		if (!is_lambda_function(p_function)) {
			continue;
		}
		if (!emitter.supported_lambdas.has(p_function.registry_key)) {
			continue;
		}
		const String *declaration = emitter.declaration_parameters_by_function.getptr(p_function.registry_key);
		const LambdaInfo *info = emitter.lambda_info(p_function);
		if (!declaration || !info) {
			continue;
		}
		const bool lambda_static = emitter.binding_static_by_function.getptr(p_function.registry_key) ? *emitter.binding_static_by_function.getptr(p_function.registry_key) : !info->use_self;
		out.append("\t" + String(lambda_static ? "static " : "") + entry_return_cpp(p_function) + " " + info->method + "(" + *declaration + ");\n");
	}

	out.append("};\n\n} // namespace godot\n");
	return out.as_string();
}

String GritGdxEmitter::emit_source(const GritGdxClass &p_class) {
	StringBuilder out;
	out.append("#include \"" + header_file_name(p_class) + "\"\n\n");
	out.append("#include <godot_cpp/core/class_db.hpp>\n");
	out.append("#include <godot_cpp/core/memory.hpp>\n");
	out.append("#include <godot_cpp/variant/callable_custom.hpp>\n");
	out.append("#include <godot_cpp/core/object.hpp>\n");
	out.append("#include <godot_cpp/variant/utility_functions.hpp>\n");
	out.append("#include <godot_cpp/templates/hash_map.hpp>\n");
	out.append("#include <godot_cpp/classes/ref.hpp>\n");
	out.append("#include <godot_cpp/classes/ref_counted.hpp>\n");
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
	emit_lambda_callable(p_class, emitter, out);

	for (const GritGdxProperty &property : p_class.properties) {
		const String type = cpp_storage(property.storage);
		out.append(type + " " + p_class.class_name + "::" + property.getter + "() const {\n");
		out.append("\treturn " + property.field + ";\n");
		out.append("}\n\n");
		out.append("void " + p_class.class_name + "::" + property.setter + "(" + (property.storage == Variant::NIL ? String("const Variant &p_value") : type + " p_value") + ") {\n");
		const String converted = emitter.typed_container_expression(GritType::from(property.type), "Variant(p_value)");
		out.append("\t" + property.field + " = " + (converted.is_empty() ? String("p_value") : converted) + ";\n");
		out.append("}\n\n");
	}

	bool has_ready = emitter.is_supported("_ready");
	bool needs_process = false;
	for (const GritFunction &p_function : p_class.functions) {
		if (!emitter.is_supported(String(p_function.name))) {
			continue;
		}
		if (p_function.name == StringName("_process") || p_function.name == StringName("_physics_process")) {
			needs_process = true;
		}
	}

	// Methods the backend cannot translate exist as vararg stubs so a call fails
	// loudly instead of silently doing nothing.
	const LocalVector<GritGdxSkipped> stubs = stub_methods(p_class);

	for (const GritFunction &p_function : p_class.functions) {
		const bool lambda = is_lambda_function(p_function);
		if (lambda ? !emitter.supported_lambdas.has(p_function.registry_key) : !emitter.is_supported(String(p_function.name))) {
			continue;
		}
		const bool is_virtual = !lambda && is_virtual_name(String(p_function.name));
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
		if (is_lambda_function(p_function) || is_virtual_name(String(p_function.name))) {
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
		String defaults;
		if (const LocalVector<String> *values = emitter.binding_defaults_by_function.getptr(p_function.registry_key)) {
			for (const String &value : *values) {
				defaults += ", " + value;
			}
		}
		const bool *bind_static = emitter.binding_static_by_function.getptr(p_function.registry_key);
		if (bind_static && *bind_static) {
			out.append("\tClassDB::bind_static_method(\"" + p_class.class_name + "\", D_METHOD(" + escaped_string(sanitize(String(p_function.name))) + (names.is_empty() ? String() : ", " + names) + "), &" + p_class.class_name + "::" + sanitize(String(p_function.name)) + defaults + ");\n");
		} else {
			out.append("\tClassDB::bind_method(D_METHOD(" + escaped_string(sanitize(String(p_function.name))) + (names.is_empty() ? String() : ", " + names) + "), &" + p_class.class_name + "::" + sanitize(String(p_function.name)) + defaults + ");\n");
		}
	}
	for (const GritGdxSkipped &entry : stubs) {
		const String stub = "_grit_unconverted_" + sanitize(String(entry.name));
		out.append("\t{\n");
		out.append("\t\tMethodInfo stub_info;\n");
		out.append("\t\tstub_info.name = " + escaped_string(String(entry.name)) + ";\n");
		out.append("\t\tClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, " + escaped_string(String(entry.name)) + ", &" + p_class.class_name + "::" + stub + ", stub_info);\n");
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
		if (is_lambda_function(function_entry) || emitter.is_supported(String(function_entry.name))) {
			continue;
		}
		emitter.reject_reason = String();
		StringBuilder probe;
		emitter.emit_function(function_entry, is_virtual_name(String(function_entry.name)), probe);
		GritGdxSkipped entry;
		entry.name = String(function_entry.name);
		entry.reason = emitter.reject_reason.is_empty() ? vformat("not supported (opcode %d, line %d)", emitter.last_opcode, emitter.last_statement_line) : emitter.reject_reason;
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
	helpers.append("#include <godot_cpp/classes/script.hpp>\n");
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
	// Building a typed container: set_typed only works on a fresh, empty one, so
	// the elements are copied in afterwards (which validates them).
	helpers.append("inline Array typed_array(const Variant &p_value, int64_t p_builtin_type, const StringName &p_native_type) {\n");
	helpers.append("\tif (p_value.get_type() == Variant::ARRAY) {\n");
	helpers.append("\t\tArray source = p_value;\n");
	helpers.append("\t\tif (source.is_typed() && source.get_typed_builtin() == p_builtin_type && source.get_typed_class_name() == p_native_type && source.get_typed_script().operator Object *() == nullptr) {\n");
	helpers.append("\t\t\treturn source;\n");
	helpers.append("\t\t}\n");
	helpers.append("\t}\n");
	helpers.append("\tArray result;\n");
	helpers.append("\tresult.set_typed(p_builtin_type, p_native_type, Variant());\n");
	helpers.append("\tif (p_value.get_type() == Variant::ARRAY) {\n");
	helpers.append("\t\tArray source = p_value;\n");
	helpers.append("\t\tfor (int64_t i = 0; i < source.size(); i++) {\n");
	helpers.append("\t\t\tresult.push_back(source[i]);\n");
	helpers.append("\t\t}\n");
	helpers.append("\t} else if (p_value.get_type() != Variant::NIL) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: cannot convert a non-array value to a typed array.\");\n");
	helpers.append("\t}\n");
	helpers.append("\treturn result;\n");
	helpers.append("}\n\n");
	helpers.append("inline Dictionary typed_dictionary(const Variant &p_value, int64_t p_key_builtin, const StringName &p_key_native, int64_t p_value_builtin, const StringName &p_value_native) {\n");
	helpers.append("\tif (p_value.get_type() == Variant::DICTIONARY) {\n");
	helpers.append("\t\tDictionary source = p_value;\n");
	helpers.append("\t\tif (source.is_typed() && source.get_typed_key_builtin() == p_key_builtin && source.get_typed_key_class_name() == p_key_native && source.get_typed_key_script().operator Object *() == nullptr &&\n");
	helpers.append("\t\t\t\tsource.get_typed_value_builtin() == p_value_builtin && source.get_typed_value_class_name() == p_value_native && source.get_typed_value_script().operator Object *() == nullptr) {\n");
	helpers.append("\t\t\treturn source;\n");
	helpers.append("\t\t}\n");
	helpers.append("\t}\n");
	helpers.append("\tDictionary result;\n");
	helpers.append("\tresult.set_typed(p_key_builtin, p_key_native, Variant(), p_value_builtin, p_value_native, Variant());\n");
	helpers.append("\tif (p_value.get_type() == Variant::DICTIONARY) {\n");
	helpers.append("\t\tDictionary source = p_value;\n");
	helpers.append("\t\tArray keys = source.keys();\n");
	helpers.append("\t\tfor (int64_t i = 0; i < keys.size(); i++) {\n");
	helpers.append("\t\t\tVariant key = keys[i];\n");
	helpers.append("\t\t\tresult[key] = source[key];\n");
	helpers.append("\t\t}\n");
	helpers.append("\t} else if (p_value.get_type() != Variant::NIL) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: cannot convert a non-dictionary value to a typed dictionary.\");\n");
	helpers.append("\t}\n");
	helpers.append("\treturn result;\n");
	helpers.append("}\n\n");
	helpers.append("inline bool is_script(const Variant &p_value, const String &p_path) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::OBJECT) {\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\tObject *object = p_value;\n");
	helpers.append("\tif (object == nullptr) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: the left operand of 'is' is a previously freed instance.\");\n");
	helpers.append("\t\treturn false;\n");
	helpers.append("\t}\n");
	helpers.append("\tRef<Script> script = object->get_script();\n");
	helpers.append("\twhile (script.is_valid()) {\n");
	helpers.append("\t\tif (script->get_path() == p_path) {\n");
	helpers.append("\t\t\treturn true;\n");
	helpers.append("\t\t}\n");
	helpers.append("\t\tscript = script->get_base_script();\n");
	helpers.append("\t}\n");
	helpers.append("\treturn false;\n");
	helpers.append("}\n\n");
	helpers.append("inline Variant cast_script(const Variant &p_value, const String &p_path) {\n");
	helpers.append("\tif (p_value.get_type() != Variant::OBJECT && p_value.get_type() != Variant::NIL) {\n");
	helpers.append("\t\tUtilityFunctions::push_error(\"Grit: cannot convert a non-object value to a script type.\");\n");
	helpers.append("\t\treturn Variant();\n");
	helpers.append("\t}\n");
	helpers.append("\treturn is_script(p_value, p_path) ? p_value : Variant();\n");
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
	helpers.append("} // namespace grit_gdx\n\n");
	return helpers.as_string();
}
