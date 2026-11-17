#pragma once

#include "grit_gdx_class.h"

#include "core/string/string_builder.h"

// Emits a godot-cpp GDExtension class from Grit IR.
//
// The generated code targets the public GDExtension API (godot-cpp) instead of
// engine internals: script functions become C++ methods, script variables
// become fields, and exports/signals become ClassDB registrations.
class GritGdxEmitter {
public:
	static String emit_header(const GritGdxClass &p_class);
	static String emit_source(const GritGdxClass &p_class);
	static String header_file_name(const GritGdxClass &p_class);
	// Functions the backend cannot express, with the reason, so the caller can
	// report them instead of silently leaving them behind.
	static LocalVector<GritGdxSkipped> skipped_functions(const GritGdxClass &p_class);
	// Small runtime shared by every generated class (operator evaluation,
	// keyed access, literals).
	static String emit_helpers(const GritGdxClass &p_class);
	static String helpers_file_name();

private:
	// One field of a generated coroutine state object.
	struct StateField {
		String type;
		String name;
		String initializer;
	};

	explicit GritGdxEmitter(const GritGdxClass &p_class);

	const GritGdxClass &klass;
	const GritFunction *function = nullptr;
	StringBuilder body;
	int indentation = 1;
	int symbol_count = 0;
	HashMap<String, const GritFunction *> methods;
	HashMap<String, const GritGdxProperty *> properties_by_name;
	HashMap<String, bool> supported_methods;
	String reject_reason;

	// Coroutine (await) lowering: a function that awaits becomes an entry
	// method that allocates a state object, a step method that runs until the
	// next suspension, and a vararg resume method driven by the signal.
	bool coroutine = false;
	String state_struct_name;
	String state_step_method;
	String state_resume_method;
	String state_map_name;
	LocalVector<StateField> state_fields;
	HashMap<StringName, LocalVector<StateField>> await_state_fields;

	// Trial-emits every function once so header, bindings and call sites agree
	// on which methods actually exist in the generated class.
	void prepare();
	bool is_supported(const String &p_name) const;

	static bool is_virtual_name(const String &p_name);
	static String virtual_signature_of(const GritFunction &p_function);
	static String function_return_cpp(const GritFunction &p_function);
	String return_type_of(const GritFunction &p_function) const;
	static String function_parameter_cpp(const GritVariable &p_variable, const String &p_name);

	void line(const String &p_text);
	void reject(const String &p_reason);

	static String snake_case(const String &p_name);
	static String sanitize(const String &p_name);
	static String cpp_storage(Variant::Type p_type);
	static String default_storage_value(Variant::Type p_type);
	static String escaped_string(const String &p_text);
	static String float_literal(double p_value);
	static String default_literal(const Variant &p_value, Variant::Type p_storage);

	String variable_name(int p_variable) const;
	String variable_base_name(int p_variable) const;
	String state_reference(const String &p_name) const;
	void add_state_field(const String &p_type, const String &p_name, const String &p_initializer);
	static String await_struct_name(const String &p_function);
	static String await_step_method(const String &p_function);
	static String await_resume_method(const String &p_function);
	static String await_map_name(const String &p_function);
	const GritGdxProperty *property_of(int p_member) const;
	const GritFunction *method_named(const String &p_name) const;
	Variant::Type storage_of(const GritOperand &p_operand) const;

	String constant_expression(int p_constant);
	String read(const GritOperand &p_operand);
	String read_as(const GritOperand &p_operand, Variant::Type p_type);
	String read_any(const GritOperand &p_operand);
	String lvalue(const GritOperand &p_operand);
	String convert(const String &p_expression, Variant::Type p_from, Variant::Type p_to) const;
	String dynamic_operator(Variant::Operator p_operator, const GritOperand &p_left, const GritOperand &p_right);

	bool write_block(int p_block);
	bool write_statement(const GritStatement &p_statement);
	bool write_await(const GritStatement &p_statement);
	bool write_operator(const GritStatement &p_statement);
	bool write_access(const GritStatement &p_statement);
	bool write_call(const GritStatement &p_statement);
	bool write_control(const GritStatement &p_statement);
	bool write_return(const GritStatement &p_statement);
	String call_arguments(const GritStatement &p_statement, int p_first, const GritFunction *p_target);

	bool emit_function(const GritFunction &p_function, bool p_is_virtual, StringBuilder &r_output);
};
