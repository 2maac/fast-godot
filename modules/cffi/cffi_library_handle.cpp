#include "cffi_compat.hpp"

#include "cffi_library_handle.hpp"
#include "cffi_function.hpp"
#include "cffi_pointer.hpp"
#include "cffi_type.hpp"

#include "core/os/os.h"

// The original GDExtension's SConstruct defined this per platform; a module
// build does not, so derive it here.
#ifndef SHLIBSUFFIX
	#if defined(_WIN32)
		#define SHLIBSUFFIX ".dll"
	#elif defined(__APPLE__)
		#define SHLIBSUFFIX ".dylib"
	#else
		#define SHLIBSUFFIX ".so"
	#endif
#endif

#ifdef _WIN32
	#include <windows.h>
	#include <system_error>
#else
	#include <dlfcn.h>
#endif

static void *os_open_library(const char *path) {
#ifdef _WIN32
	return LoadLibrary(path);
#else
	return dlopen(path, RTLD_LAZY);
#endif
}

static String os_get_last_error() {
#ifdef _WIN32
	auto str = std::system_category().message(GetLastError());
	return String::utf8(str.c_str(), str.size());
#else
	return dlerror();
#endif
}

static int os_close_library(void *handle) {
#ifdef _WIN32
	return FreeLibrary((HMODULE) handle);
#else
	return dlclose(handle);
#endif
}

static void *os_get_symbol(void *handle, const char *symbol) {
#ifdef _WIN32
	return (void *) GetProcAddress((HMODULE) handle, symbol);
#else
	return dlsym(handle, symbol);
#endif
}

namespace cffi {

CFFILibraryHandle::CFFILibraryHandle() {}
CFFILibraryHandle::CFFILibraryHandle(void *library_handle) : library_handle(library_handle) {}

CFFILibraryHandle::~CFFILibraryHandle() {
	if (library_handle) {
		os_close_library(library_handle);
	}
}

Ref<CFFILibraryHandle> CFFILibraryHandle::open(const String& name) {
	// Plain filesystem paths only. `res://` lives inside the pack, which dlopen
	// cannot read, so callers resolve real paths themselves.
	String path = name;
	if (!name.is_empty() && !name.contains(".")) {
		path += SHLIBSUFFIX;
	}
	void *library_handle = os_open_library(path.is_empty() ? nullptr : path.utf8().get_data());
	ERR_FAIL_COND_V_MSG(library_handle == nullptr, nullptr, os_get_last_error());
	return memnew(CFFILibraryHandle(library_handle));
}

Ref<CFFIFunction> CFFILibraryHandle::get_function(const String& name, const Variant& return_type_var, const Array& argument_types_arr, bool is_variadic) const {
	void *address = os_get_symbol(library_handle, name.ascii().get_data());
	ERR_FAIL_COND_V_MSG(address == nullptr, nullptr, os_get_last_error());

	Ref<CFFIType> return_type = CFFIType::from_variant(return_type_var, this);
	ERR_FAIL_COND_V_MSG(return_type == nullptr, nullptr, String("Could not find return type: %s") % return_type_var.stringify());

	CFFITypeTuple argument_types = CFFITypeTuple::from_array(argument_types_arr, this);
	ERR_FAIL_COND_V_MSG(argument_types.size() != argument_types_arr.size(), nullptr, "Invalid argument types");
	return memnew(CFFIFunction(name, address, return_type, argument_types, is_variadic));
}

Ref<CFFIPointer> CFFILibraryHandle::get_global(const String& name, const Variant& type_var) {
	void *address = os_get_symbol(library_handle, name.ascii().get_data());
	ERR_FAIL_COND_V_MSG(address == nullptr, nullptr, os_get_last_error());

	auto type = CFFIType::from_variant(type_var, this);
	ERR_FAIL_COND_V_MSG(type == nullptr, nullptr, String("Could not find type: %s") % type_var.stringify());

	return memnew(CFFIPointer(type, (uint8_t *) address));
}

void CFFILibraryHandle::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_function", "name", "return_type", "argument_types", "is_variadic"), &CFFILibraryHandle::get_function, DEFVAL(Array()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_global", "name", "type"), &CFFILibraryHandle::get_global);
}

String CFFILibraryHandle::_to_string() {
	return String("[%s:0x%x]") % make_array(get_class(), (uint64_t) library_handle);
}

}