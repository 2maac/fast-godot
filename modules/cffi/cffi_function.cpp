#include "cffi_compat.hpp"

#include "cffi_array_type.hpp"
#include "cffi_function.hpp"
#include "cffi_pointer_type.hpp"
#include "cffi_type.hpp"
#include "cffi_value_tuple.hpp"

namespace cffi {

CFFIFunction::CFFIFunction() {}
CFFIFunction::CFFIFunction(const String& name, void *address, const Ref<CFFIType>& return_type, const CFFITypeTuple& argument_types, bool is_variadic, ffi_abi abi)
	: name(name)
	, address(address)
	, return_type(return_type)
	, argument_types(argument_types)
	, is_variadic(is_variadic)
{
	// decay arrays into pointer
	for (auto& arg_type : this->argument_types.get_fields()) {
		if (auto array_type = Object::cast_to<CFFIArrayType>(arg_type.ptr())) {
			arg_type = Ref(memnew(CFFIPointerType(array_type->get_element_type())));
		}
	}
	ffi_status status = ffi_prep_cif(&ffi_handle, abi, argument_types.size(), &return_type->get_ffi_type(), this->argument_types.get_element_types());
	ERR_FAIL_COND(status != FFI_OK);
}

Variant CFFIFunction::invoke(const CFFIValueTuple& argument_data) {
	// The FFI reads one pointer per declared argument straight out of this tuple.
	// A tuple that failed to convert has no addresses at all, so calling with it
	// would pass a null argument vector to ffi_call.
	ERR_FAIL_COND_V_MSG(argument_data.size() != argument_types.size(), Variant(),
		String("%s expects %d arguments, but %d were converted")
			% make_array(name, (int64_t) argument_types.size(), (int64_t) argument_data.size()));
	// Where ffi_call writes the return value. This used to be a PackedByteArray
	// resized on every call, which heap-allocated for even a 4-byte int return -
	// and that is the ONE allocation on this path that is the caller's return
	// value, paid per returned value. Every scalar type and any struct up to 16
	// bytes now fits in a stack buffer, so only a genuinely large return reaches
	// the heap.
	//
	// The floor matters here: sizeof(ffi_arg) is 8, and libffi is entitled to
	// write a full ffi_arg for a narrower return, which is why the old code
	// asked for MAX(size, sizeof(ffi_arg)) and why the buffer below is 16.
	static constexpr int64_t INLINE_RETURN_BYTES = 16;
	uint8_t inline_return[INLINE_RETURN_BYTES];
	PackedByteArray heap_return;
	uint8_t *return_data = inline_return;
	const int64_t return_size = return_type->get_size();
	if (return_size > INLINE_RETURN_BYTES) {
		heap_return.resize(return_size);
		return_data = heap_return.ptrw();
	}
	ffi_call(&ffi_handle, (void(*)()) address, (void *) return_data, (void **) argument_data.get_value_addresses());
	Variant return_value;
	bool return_type_valid = return_type->data_to_variant(return_data, return_value);
	ERR_FAIL_COND_V_MSG(!return_type_valid, Variant(), "Return type is not supported");
	return return_value;
}

Variant CFFIFunction::invokev(const Array& arguments) {
	if (arguments.size() < argument_types.size()) {
		ERR_FAIL_V_EDMSG(Variant(), String("Expected %d arguments.") % (int64_t) argument_types.size());
	}
	else if (!is_variadic && arguments.size() > argument_types.size()) {
		ERR_FAIL_V_EDMSG(Variant(), String("Expected %d arguments.") % (int64_t) argument_types.size());
	}

	CFFIValueTuple argument_data = CFFIValueTuple::from_array(argument_types, arguments);
	ERR_FAIL_COND_V_EDMSG(!argument_data.is_valid(), Variant(), _argument_error_message(argument_data));
	return invoke(argument_data);
}

Variant CFFIFunction::invoke_variadic(const Variant **args, int arg_count, Callable::CallError &error) {
	if (arg_count < argument_types.size()) {
		error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		error.expected = argument_types.size();
		return Variant();
	}
	else if (!is_variadic && arg_count > argument_types.size()) {
		error.error = Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS;
		error.expected = argument_types.size();
		return Variant();
	}

	CFFIValueTuple argument_data = CFFIValueTuple::from_varargs(argument_types, args, arg_count);
	if (!argument_data.is_valid()) {
		error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		error.argument = argument_data.get_error_index();
		error.expected = argument_types.size();
		return Variant();
	}
	return invoke(argument_data);
}

String CFFIFunction::_argument_error_message(const CFFIValueTuple& argument_data) const {
	int64_t index = argument_data.get_error_index();
	if (index < 0 || index >= argument_types.size()) {
		return String("%s: arguments could not be converted") % name;
	}
	return String("%s: argument %d cannot be converted to %s")
		% make_array(name, index, argument_types.get_fields()[index]->get_name());
}

void *CFFIFunction::get_code_address() const {
	return address;
}

void CFFIFunction::_bind_methods() {
	ClassDB::bind_method(D_METHOD("invokev", "args"), &CFFIFunction::invokev);
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "invoke", &CFFIFunction::invoke_variadic);
}

String CFFIFunction::_to_string() const {
	return String("[%s:0x%x:%s %s%s]") % make_array(get_class(), (uint64_t) address, return_type->get_name(), name, argument_types.to_string());
}

}