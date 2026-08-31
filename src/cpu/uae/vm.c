#include "sysdeps.h"
#include "uae/vm.h"

#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(CPU_AARCH64)
#include <pthread.h>
#endif

static int native_protection(int protection)
{
	int result = PROT_NONE;
	if (protection & UAE_VM_READ)
		result |= PROT_READ;
	if (protection & UAE_VM_WRITE)
		result |= PROT_WRITE;
	if (protection & UAE_VM_EXECUTE)
		result |= PROT_EXEC;
	return result;
}

void *uae_vm_alloc(size_t size, int flags, int protection)
{
	int mmap_flags = MAP_PRIVATE | MAP_ANON;
#if defined(__APPLE__) && defined(CPU_AARCH64) && defined(MAP_JIT)
	if (flags & UAE_VM_JIT)
		mmap_flags |= MAP_JIT;
#else
	(void)flags;
#endif
	void *result = mmap(NULL, size, native_protection(protection), mmap_flags, -1, 0);
	return result == MAP_FAILED ? NULL : result;
}

bool uae_vm_protect(void *address, size_t size, int protection)
{
	return mprotect(address, size, native_protection(protection)) == 0;
}

bool uae_vm_free(void *address, size_t size)
{
	return munmap(address, size) == 0;
}

void uae_vm_jit_write_protect(bool enable_execute_mode)
{
#if defined(__APPLE__) && defined(CPU_AARCH64)
	pthread_jit_write_protect_np(enable_execute_mode ? 1 : 0);
#else
	(void)enable_execute_mode;
#endif
}

int uae_vm_page_size(void)
{
	return (int)sysconf(_SC_PAGESIZE);
}
