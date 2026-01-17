/*
  Previous - sdlhost.c

  This file is distributed under the GNU General Public License, version 2
  or at your option any later version. Read the file gpl.txt for details.

  Host system dependencies such as time synchronization and events that
  occur on host threads (e.g. VBL & timers) which need to be synchronized
  to Previous CPU threads.
*/
const char SDLhost_fileid[] = "Previous sdlhost.c";

#include "config.h"

#if HAVE_NANOSLEEP
#include <errno.h>
#ifdef __MINGW32__
#include <unistd.h>
#else
#include <sys/time.h>
#endif
#endif
#include "host.h"


void host_lock(lock_t* lock) {
	SDL_AtomicLock(lock);
}

int host_trylock(lock_t* lock) {
	return SDL_AtomicTryLock(lock);
}

void host_unlock(lock_t* lock) {
	SDL_AtomicUnlock(lock);
}

int host_atomic_set(atomic_int* a, int newValue) {
	return SDL_AtomicSet(a, newValue);
}

int host_atomic_get(atomic_int* a) {
	return SDL_AtomicGet(a);
}

int host_atomic_add(atomic_int* a, int value) {
	return SDL_AtomicAdd(a, value);
}

int host_atomic_cas(atomic_int* a, int oldValue, int newValue) {
	return SDL_AtomicCAS(a, oldValue, newValue);
}

thread_t* host_thread_create(thread_func_t func, const char* name, void* data) {
	return SDL_CreateThread(func, name, data);
}

void host_thread_priority(int priority) {
	SDL_ThreadPriority val;
	switch (priority) {
		case 0:  val = SDL_THREAD_PRIORITY_LOW; break;
		case 1:  val = SDL_THREAD_PRIORITY_NORMAL; break;
		case 2:  val = SDL_THREAD_PRIORITY_HIGH; break;
		case 3:  val = SDL_THREAD_PRIORITY_TIME_CRITICAL; break;
		default: val = SDL_THREAD_PRIORITY_NORMAL; break;
	}
	SDL_SetThreadPriority(val);
}

int host_thread_wait(thread_t* thread) {
	int status;
	SDL_WaitThread(thread, &status);
	return status;
}

semaphore_t* host_semaphore_create(uint32_t value) {
	return SDL_CreateSemaphore(value);
}

void host_semaphore_signal(semaphore_t* semaphore) {
	SDL_SemPost(semaphore);
}

int host_semaphore_wait_timeout(semaphore_t* semaphore, int32_t ms) {
	return SDL_SemWaitTimeout(semaphore, ms);
}

void host_semaphore_destroy(semaphore_t* semaphore) {
	SDL_DestroySemaphore(semaphore);
}

mutex_t* host_mutex_create(void) {
	return SDL_CreateMutex();
}

void host_mutex_lock(mutex_t* mutex) {
	SDL_LockMutex(mutex);
}

void host_mutex_unlock(mutex_t* mutex) {
	SDL_UnlockMutex(mutex);
}

void host_mutex_destroy(mutex_t* mutex) {
	SDL_DestroyMutex(mutex);
}

void host_sleep_ms(uint32_t ms) {
	SDL_Delay(ms);
}

void host_sleep_us(uint64_t us) {
#if HAVE_NANOSLEEP
	struct timespec	ts;
	int ret;
	ts.tv_sec = us / 1000000ULL;
	ts.tv_nsec = (us % 1000000ULL) * 1000;	/* micro sec -> nano sec */
	/* wait until all the delay is elapsed, including possible interruptions by signals */
	do {
		errno = 0;
		ret = nanosleep(&ts, &ts);
	} while (ret && (errno == EINTR));		/* keep on sleeping if we were interrupted */
#else
	uint64_t timeout = us;
	timeout += real_time();
	host_sleep_ms((uint32_t)(us / 1000ULL));
	while (real_time() < timeout) {}
#endif
}

uint64_t host_get_counter(void) {
	return SDL_GetPerformanceCounter();
}

uint64_t host_get_counter_frequency(void) {
	return SDL_GetPerformanceFrequency();
}

int host_num_cpus(void) {
	return SDL_GetCPUCount();
}
