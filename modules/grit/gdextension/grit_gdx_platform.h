#pragma once

#include "core/string/ustring.h"

// Host platform/architecture keys used in GDExtension descriptors and file names.
static inline String grit_gdx_platform_key() {
#if defined(MACOS_ENABLED)
	return "macos";
#elif defined(WINDOWS_ENABLED)
	return "windows";
#else
	return "linux";
#endif
}

static inline String grit_gdx_arch_key() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
	return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
	return "x86_64";
#else
	return "unknown";
#endif
}
