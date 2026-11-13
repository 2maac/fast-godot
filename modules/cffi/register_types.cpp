#include "cffi_compat.hpp"

#include "register_types.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"

#include "cffi.hpp"
#include "cffi_array_type.hpp"
#include "cffi_callable_function.hpp"
#include "cffi_function.hpp"
#include "cffi_library_handle.hpp"
#include "cffi_owned_array.hpp"
#include "cffi_owned_value.hpp"
#include "cffi_pointer.hpp"
#include "cffi_pointer_type.hpp"
#include "cffi_scope.hpp"
#include "cffi_span.hpp"
#include "cffi_struct_type.hpp"
#include "cffi_type.hpp"
#include "stream_peer_cffi_pointer.hpp"

using namespace cffi;

void initialize_cffi_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_ABSTRACT_CLASS(CFFIScope);
	GDREGISTER_ABSTRACT_CLASS(CFFILibraryHandle);
	GDREGISTER_ABSTRACT_CLASS(CFFIFunction);
	GDREGISTER_ABSTRACT_CLASS(CFFICallableFunction);
	GDREGISTER_ABSTRACT_CLASS(CFFIType);
	GDREGISTER_ABSTRACT_CLASS(CFFIArrayType);
	GDREGISTER_ABSTRACT_CLASS(CFFIPointerType);
	GDREGISTER_ABSTRACT_CLASS(CFFIStructType);
	GDREGISTER_ABSTRACT_CLASS(CFFIPointer);
	GDREGISTER_ABSTRACT_CLASS(CFFISpan);
	GDREGISTER_ABSTRACT_CLASS(CFFIOwnedArray);
	GDREGISTER_ABSTRACT_CLASS(CFFIOwnedValue);
	GDREGISTER_ABSTRACT_CLASS(CFFI);
	GDREGISTER_CLASS(StreamPeerCFFIPointer);
	CFFI::get_or_create_singleton();
}

void uninitialize_cffi_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	CFFI::delete_singleton();
}
