#pragma once

#include "grit_gdx_class.h"

#include "core/string/string_builder.h"
#include "core/templates/hash_set.h"

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
	// Skipped methods that get a stub (everything but virtuals and the implicit
	// GDScript accessors), so a call fails loudly at runtime.
	static LocalVector<GritGdxSkipped> stub_methods(const GritGdxClass &p_class);
	// Small runtime shared by every generated class (operator evaluation,
	// keyed access, literals).
	static String emit_helpers(const GritGdxClass &p_class);
	static String helpers_file_name();

private:
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

	HashMap<StringName, LocalVector<String>> binding_defaults_by_function;
	HashMap<StringName, String> declaration_parameters_by_function;
	HashMap<StringName, bool> binding_static_by_function;
	// Static methods have no `this`; the body must not touch members or self.
	bool static_function = false;

	// A lambda is emitted as an internal method; a creation site builds a
	// CallableCustom that calls it with the captured values prepended.
	struct LambdaInfo {
		String method;
		int kind = 0;
		bool use_self = false;
	};
	struct LambdaSite {
		String method;
		String key;
		int kind = 0;
		bool use_self = false;
		int captures = 0;
		int declared = 0;
	};
	HashMap<String, LambdaInfo> lambdas_by_key;
	HashMap<const GritStatement *, LambdaSite> lambda_sites;
	HashSet<String> supported_lambdas;
	bool lambda_function = false;

	static bool is_lambda_function(const GritFunction &p_function);
	static int lambda_key_line(const String &p_key);
	void prepare_lambdas();
	const LambdaInfo *lambda_info(const GritFunction &p_function) const;
	int last_opcode = -1;
	int last_statement_line = 0;

	// Trial-emits every function once so header, bindings and call sites agree
	// on which methods actually exist in the generated class.
	void prepare();
	bool is_supported(const String &p_name) const;

	static bool is_virtual_name(const String &p_name);
	static String virtual_signature_of(const GritFunction &p_function);
	static String function_return_cpp(const GritFunction &p_function);
	static String entry_return_cpp(const GritFunction &p_function);
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
	bool write_assert(const GritStatement &p_statement);
	bool write_create_lambda(const GritStatement &p_statement);
	static void emit_lambda_callable(const GritGdxClass &p_class, const GritGdxEmitter &p_emitter, StringBuilder &r_out);
	void write_abort_return();
	bool write_type_test(const GritStatement &p_statement);
	bool write_cast(const GritStatement &p_statement);
	bool container_type_test(const GritType &p_type, const String &p_value, String &r_expression);
	GritType declared_type_of(const GritOperand &p_operand) const;
	bool optional_parameter_default(const GritFunction &p_function, int p_parameter_variable, String &r_expression);
	bool container_element_arguments(const GritType &p_element, String &r_builtin, String &r_native);
	String typed_container_expression(const GritType &p_type, const String &p_value);
	bool write_operator(const GritStatement &p_statement);
	bool write_access(const GritStatement &p_statement);
	bool write_call(const GritStatement &p_statement);
	bool write_control(const GritStatement &p_statement);
	bool write_return(const GritStatement &p_statement);
	String call_arguments(const GritStatement &p_statement, int p_first, const GritFunction *p_target);

	bool emit_function(const GritFunction &p_function, bool p_is_virtual, StringBuilder &r_output);
};
