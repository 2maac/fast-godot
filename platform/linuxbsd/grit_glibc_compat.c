/**************************************************************************/
/*  grit_glibc_compat.c                                                   */
/**************************************************************************/
/*                         Fast_Godot fork                               */
/**************************************************************************/

// glibc only gained a memfd_create() wrapper in 2.27. PCRE2's sljit JIT calls it
// to allocate executable memory, so a template built against an older glibc (the
// bundled Zig compiler targets 2.17 for wide distribution) failed to link.
//
// The kernel syscall has existed since 3.17, so calling it directly is enough. A
// definition in the executable also wins over the libc symbol on newer systems,
// where it does exactly the same thing.

#include <errno.h>
#include <sys/syscall.h>
#include <unistd.h>

int memfd_create(const char *p_name, unsigned int p_flags) {
#ifdef SYS_memfd_create
	return (int)syscall(SYS_memfd_create, p_name, p_flags);
#else
	errno = ENOSYS;
	return -1;
#endif
}
