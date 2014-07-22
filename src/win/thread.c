/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "uv.h"
#include "internal.h"


#define HAVE_SRWLOCK_API() (pTryAcquireSRWLockShared != NULL)
#define HAVE_CONDVAR_API() (pInitializeConditionVariable != NULL)

#ifdef _MSC_VER /* msvc */
# define inline __inline
# define NOINLINE __declspec (noinline)
#else  /* gcc */
# define inline inline
# define NOINLINE __attribute__ ((noinline))
#endif

static QUEUE dtor_queue; /* TLS destructors*/
static uv_once_t once = UV_ONCE_INIT;
static uv_mutex_t mutex;

inline static int uv__rwlock_srwlock_init(uv_rwlock_t* rwlock);
inline static void uv__rwlock_srwlock_destroy(uv_rwlock_t* rwlock);
inline static void uv__rwlock_srwlock_rdlock(uv_rwlock_t* rwlock);
inline static int uv__rwlock_srwlock_tryrdlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_srwlock_rdunlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_srwlock_wrlock(uv_rwlock_t* rwlock);
inline static int uv__rwlock_srwlock_trywrlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_srwlock_wrunlock(uv_rwlock_t* rwlock);

inline static int uv__rwlock_fallback_init(uv_rwlock_t* rwlock);
inline static void uv__rwlock_fallback_destroy(uv_rwlock_t* rwlock);
inline static void uv__rwlock_fallback_rdlock(uv_rwlock_t* rwlock);
inline static int uv__rwlock_fallback_tryrdlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_fallback_rdunlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_fallback_wrlock(uv_rwlock_t* rwlock);
inline static int uv__rwlock_fallback_trywrlock(uv_rwlock_t* rwlock);
inline static void uv__rwlock_fallback_wrunlock(uv_rwlock_t* rwlock);


inline static int uv_cond_fallback_init(uv_cond_t* cond);
inline static void uv_cond_fallback_destroy(uv_cond_t* cond);
inline static void uv_cond_fallback_signal(uv_cond_t* cond);
inline static void uv_cond_fallback_broadcast(uv_cond_t* cond);
inline static void uv_cond_fallback_wait(uv_cond_t* cond, uv_mutex_t* mutex);
inline static int uv_cond_fallback_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout);

inline static int uv_cond_condvar_init(uv_cond_t* cond);
inline static void uv_cond_condvar_destroy(uv_cond_t* cond);
inline static void uv_cond_condvar_signal(uv_cond_t* cond);
inline static void uv_cond_condvar_broadcast(uv_cond_t* cond);
inline static void uv_cond_condvar_wait(uv_cond_t* cond, uv_mutex_t* mutex);
inline static int uv_cond_condvar_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout);


static NOINLINE void uv__once_inner(uv_once_t* guard,
    void (*callback)(void)) {
  DWORD result;
  HANDLE existing_event, created_event;

  created_event = CreateEvent(NULL, 1, 0, NULL);
  if (created_event == 0) {
    /* Could fail in a low-memory situation? */
    uv_fatal_error(GetLastError(), "CreateEvent");
  }

  existing_event = InterlockedCompareExchangePointer(&guard->event,
                                                     created_event,
                                                     NULL);

  if (existing_event == NULL) {
    /* We won the race */
    callback();

    result = SetEvent(created_event);
    assert(result);
    guard->ran = 1;

  } else {
    /* We lost the race. Destroy the event we created and wait for the */
    /* existing one todv become signaled. */
    CloseHandle(created_event);
    result = WaitForSingleObject(existing_event, INFINITE);
    assert(result == WAIT_OBJECT_0);
  }
}


void uv_once(uv_once_t* guard, void (*callback)(void)) {
  /* Fast case - avoid WaitForSingleObject. */
  if (guard->ran) {
    return;
  }

  uv__once_inner(guard, callback);
}


int uv_thread_join(uv_thread_t *tid) {
  if (WaitForSingleObject(*tid, INFINITE))
    return uv_translate_sys_error(GetLastError());
  else {
    CloseHandle(*tid);
    *tid = 0;
    return 0;
  }
}


int uv_mutex_init(uv_mutex_t* mutex) {
  InitializeCriticalSection(mutex);
  return 0;
}


void uv_mutex_destroy(uv_mutex_t* mutex) {
  DeleteCriticalSection(mutex);
}


void uv_mutex_lock(uv_mutex_t* mutex) {
  EnterCriticalSection(mutex);
}


int uv_mutex_trylock(uv_mutex_t* mutex) {
  if (TryEnterCriticalSection(mutex))
    return 0;
  else
    return UV_EAGAIN;
}


void uv_mutex_unlock(uv_mutex_t* mutex) {
  LeaveCriticalSection(mutex);
}


int uv_rwlock_init(uv_rwlock_t* rwlock) {
  uv__once_init();

  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_init(rwlock);
  else
    return uv__rwlock_fallback_init(rwlock);
}


void uv_rwlock_destroy(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_destroy(rwlock);
  else
    uv__rwlock_fallback_destroy(rwlock);
}


void uv_rwlock_rdlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_rdlock(rwlock);
  else
    uv__rwlock_fallback_rdlock(rwlock);
}


int uv_rwlock_tryrdlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_tryrdlock(rwlock);
  else
    return uv__rwlock_fallback_tryrdlock(rwlock);
}


void uv_rwlock_rdunlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_rdunlock(rwlock);
  else
    uv__rwlock_fallback_rdunlock(rwlock);
}


void uv_rwlock_wrlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_wrlock(rwlock);
  else
    uv__rwlock_fallback_wrlock(rwlock);
}


int uv_rwlock_trywrlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    return uv__rwlock_srwlock_trywrlock(rwlock);
  else
    return uv__rwlock_fallback_trywrlock(rwlock);
}


void uv_rwlock_wrunlock(uv_rwlock_t* rwlock) {
  if (HAVE_SRWLOCK_API())
    uv__rwlock_srwlock_wrunlock(rwlock);
  else
    uv__rwlock_fallback_wrunlock(rwlock);
}


int uv_sem_init(uv_sem_t* sem, unsigned int value) {
  *sem = CreateSemaphore(NULL, value, INT_MAX, NULL);
  if (*sem == NULL)
    return uv_translate_sys_error(GetLastError());
  else
    return 0;
}


void uv_sem_destroy(uv_sem_t* sem) {
  if (!CloseHandle(*sem))
    abort();
}


void uv_sem_post(uv_sem_t* sem) {
  if (!ReleaseSemaphore(*sem, 1, NULL))
    abort();
}


void uv_sem_wait(uv_sem_t* sem) {
  if (WaitForSingleObject(*sem, INFINITE) != WAIT_OBJECT_0)
    abort();
}


int uv_sem_trywait(uv_sem_t* sem) {
  DWORD r = WaitForSingleObject(*sem, 0);

  if (r == WAIT_OBJECT_0)
    return 0;

  if (r == WAIT_TIMEOUT)
    return UV_EAGAIN;

  abort();
  return -1; /* Satisfy the compiler. */
}


inline static int uv__rwlock_srwlock_init(uv_rwlock_t* rwlock) {
  pInitializeSRWLock(&rwlock->srwlock_);
  return 0;
}


inline static void uv__rwlock_srwlock_destroy(uv_rwlock_t* rwlock) {
  (void) rwlock;
}


inline static void uv__rwlock_srwlock_rdlock(uv_rwlock_t* rwlock) {
  pAcquireSRWLockShared(&rwlock->srwlock_);
}


inline static int uv__rwlock_srwlock_tryrdlock(uv_rwlock_t* rwlock) {
  if (pTryAcquireSRWLockShared(&rwlock->srwlock_))
    return 0;
  else
    return UV_EBUSY;  /* TODO(bnoordhuis) EAGAIN when owned by this thread. */
}


inline static void uv__rwlock_srwlock_rdunlock(uv_rwlock_t* rwlock) {
  pReleaseSRWLockShared(&rwlock->srwlock_);
}


inline static void uv__rwlock_srwlock_wrlock(uv_rwlock_t* rwlock) {
  pAcquireSRWLockExclusive(&rwlock->srwlock_);
}


inline static int uv__rwlock_srwlock_trywrlock(uv_rwlock_t* rwlock) {
  if (pTryAcquireSRWLockExclusive(&rwlock->srwlock_))
    return 0;
  else
    return UV_EBUSY;  /* TODO(bnoordhuis) EAGAIN when owned by this thread. */
}


inline static void uv__rwlock_srwlock_wrunlock(uv_rwlock_t* rwlock) {
  pReleaseSRWLockExclusive(&rwlock->srwlock_);
}


inline static int uv__rwlock_fallback_init(uv_rwlock_t* rwlock) {
  int err;

  err = uv_mutex_init(&rwlock->fallback_.read_mutex_);
  if (err)
    return err;

  err = uv_mutex_init(&rwlock->fallback_.write_mutex_);
  if (err) {
    uv_mutex_destroy(&rwlock->fallback_.read_mutex_);
    return err;
  }

  rwlock->fallback_.num_readers_ = 0;

  return 0;
}


inline static void uv__rwlock_fallback_destroy(uv_rwlock_t* rwlock) {
  uv_mutex_destroy(&rwlock->fallback_.read_mutex_);
  uv_mutex_destroy(&rwlock->fallback_.write_mutex_);
}


inline static void uv__rwlock_fallback_rdlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.read_mutex_);

  if (++rwlock->fallback_.num_readers_ == 1)
    uv_mutex_lock(&rwlock->fallback_.write_mutex_);

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);
}


inline static int uv__rwlock_fallback_tryrdlock(uv_rwlock_t* rwlock) {
  int err;

  err = uv_mutex_trylock(&rwlock->fallback_.read_mutex_);
  if (err)
    goto out;

  err = 0;
  if (rwlock->fallback_.num_readers_ == 0)
    err = uv_mutex_trylock(&rwlock->fallback_.write_mutex_);

  if (err == 0)
    rwlock->fallback_.num_readers_++;

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);

out:
  return err;
}


inline static void uv__rwlock_fallback_rdunlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.read_mutex_);

  if (--rwlock->fallback_.num_readers_ == 0)
    uv_mutex_unlock(&rwlock->fallback_.write_mutex_);

  uv_mutex_unlock(&rwlock->fallback_.read_mutex_);
}


inline static void uv__rwlock_fallback_wrlock(uv_rwlock_t* rwlock) {
  uv_mutex_lock(&rwlock->fallback_.write_mutex_);
}


inline static int uv__rwlock_fallback_trywrlock(uv_rwlock_t* rwlock) {
  return uv_mutex_trylock(&rwlock->fallback_.write_mutex_);
}


inline static void uv__rwlock_fallback_wrunlock(uv_rwlock_t* rwlock) {
  uv_mutex_unlock(&rwlock->fallback_.write_mutex_);
}



/* This condition variable implementation is based on the SetEvent solution
 * (section 3.2) at http://www.cs.wustl.edu/~schmidt/win32-cv-1.html
 * We could not use the SignalObjectAndWait solution (section 3.4) because
 * it want the 2nd argument (type uv_mutex_t) of uv_cond_wait() and
 * uv_cond_timedwait() to be HANDLEs, but we use CRITICAL_SECTIONs.
 */

inline static int uv_cond_fallback_init(uv_cond_t* cond) {
  int err;

  /* Initialize the count to 0. */
  cond->fallback.waiters_count = 0;

  InitializeCriticalSection(&cond->fallback.waiters_count_lock);

  /* Create an auto-reset event. */
  cond->fallback.signal_event = CreateEvent(NULL,  /* no security */
                                            FALSE, /* auto-reset event */
                                            FALSE, /* non-signaled initially */
                                            NULL); /* unnamed */
  if (!cond->fallback.signal_event) {
    err = GetLastError();
    goto error2;
  }

  /* Create a manual-reset event. */
  cond->fallback.broadcast_event = CreateEvent(NULL,  /* no security */
                                               TRUE,  /* manual-reset */
                                               FALSE, /* non-signaled */
                                               NULL); /* unnamed */
  if (!cond->fallback.broadcast_event) {
    err = GetLastError();
    goto error;
  }

  return 0;

error:
  CloseHandle(cond->fallback.signal_event);
error2:
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
  return uv_translate_sys_error(err);
}


inline static int uv_cond_condvar_init(uv_cond_t* cond) {
  pInitializeConditionVariable(&cond->cond_var);
  return 0;
}


int uv_cond_init(uv_cond_t* cond) {
  uv__once_init();

  if (HAVE_CONDVAR_API())
    return uv_cond_condvar_init(cond);
  else
    return uv_cond_fallback_init(cond);
}


inline static void uv_cond_fallback_destroy(uv_cond_t* cond) {
  if (!CloseHandle(cond->fallback.broadcast_event))
    abort();
  if (!CloseHandle(cond->fallback.signal_event))
    abort();
  DeleteCriticalSection(&cond->fallback.waiters_count_lock);
}


inline static void uv_cond_condvar_destroy(uv_cond_t* cond) {
  /* nothing to do */
}


void uv_cond_destroy(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_destroy(cond);
  else
    uv_cond_fallback_destroy(cond);
}


inline static void uv_cond_fallback_signal(uv_cond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.signal_event);
}


inline static void uv_cond_condvar_signal(uv_cond_t* cond) {
  pWakeConditionVariable(&cond->cond_var);
}


void uv_cond_signal(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_signal(cond);
  else
    uv_cond_fallback_signal(cond);
}


inline static void uv_cond_fallback_broadcast(uv_cond_t* cond) {
  int have_waiters;

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  have_waiters = cond->fallback.waiters_count > 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  if (have_waiters)
    SetEvent(cond->fallback.broadcast_event);
}


inline static void uv_cond_condvar_broadcast(uv_cond_t* cond) {
  pWakeAllConditionVariable(&cond->cond_var);
}


void uv_cond_broadcast(uv_cond_t* cond) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_broadcast(cond);
  else
    uv_cond_fallback_broadcast(cond);
}


inline int uv_cond_wait_helper(uv_cond_t* cond, uv_mutex_t* mutex,
    DWORD dwMilliseconds) {
  DWORD result;
  int last_waiter;
  HANDLE handles[2] = {
    cond->fallback.signal_event,
    cond->fallback.broadcast_event
  };

  /* Avoid race conditions. */
  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count++;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* It's ok to release the <mutex> here since Win32 manual-reset events */
  /* maintain state when used with <SetEvent>. This avoids the "lost wakeup" */
  /* bug. */
  uv_mutex_unlock(mutex);

  /* Wait for either event to become signaled due to <uv_cond_signal> being */
  /* called or <uv_cond_broadcast> being called. */
  result = WaitForMultipleObjects(2, handles, FALSE, dwMilliseconds);

  EnterCriticalSection(&cond->fallback.waiters_count_lock);
  cond->fallback.waiters_count--;
  last_waiter = result == WAIT_OBJECT_0 + 1
      && cond->fallback.waiters_count == 0;
  LeaveCriticalSection(&cond->fallback.waiters_count_lock);

  /* Some thread called <pthread_cond_broadcast>. */
  if (last_waiter) {
    /* We're the last waiter to be notified or to stop waiting, so reset the */
    /* the manual-reset event. */
    ResetEvent(cond->fallback.broadcast_event);
  }

  /* Reacquire the <mutex>. */
  uv_mutex_lock(mutex);

  if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1)
    return 0;

  if (result == WAIT_TIMEOUT)
    return UV_ETIMEDOUT;

  abort();
  return -1; /* Satisfy the compiler. */
}


inline static void uv_cond_fallback_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (uv_cond_wait_helper(cond, mutex, INFINITE))
    abort();
}


inline static void uv_cond_condvar_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (!pSleepConditionVariableCS(&cond->cond_var, mutex, INFINITE))
    abort();
}


void uv_cond_wait(uv_cond_t* cond, uv_mutex_t* mutex) {
  if (HAVE_CONDVAR_API())
    uv_cond_condvar_wait(cond, mutex);
  else
    uv_cond_fallback_wait(cond, mutex);
}


inline static int uv_cond_fallback_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout) {
  return uv_cond_wait_helper(cond, mutex, (DWORD)(timeout / 1e6));
}


inline static int uv_cond_condvar_timedwait(uv_cond_t* cond,
    uv_mutex_t* mutex, uint64_t timeout) {
  if (pSleepConditionVariableCS(&cond->cond_var, mutex, (DWORD)(timeout / 1e6)))
    return 0;
  if (GetLastError() != ERROR_TIMEOUT)
    abort();
  return UV_ETIMEDOUT;
}


int uv_cond_timedwait(uv_cond_t* cond, uv_mutex_t* mutex,
    uint64_t timeout) {
  if (HAVE_CONDVAR_API())
    return uv_cond_condvar_timedwait(cond, mutex, timeout);
  else
    return uv_cond_fallback_timedwait(cond, mutex, timeout);
}


int uv_barrier_init(uv_barrier_t* barrier, unsigned int count) {
  int err;

  barrier->n = count;
  barrier->count = 0;

  err = uv_mutex_init(&barrier->mutex);
  if (err)
    return err;

  err = uv_sem_init(&barrier->turnstile1, 0);
  if (err)
    goto error2;

  err = uv_sem_init(&barrier->turnstile2, 1);
  if (err)
    goto error;

  return 0;

error:
  uv_sem_destroy(&barrier->turnstile1);
error2:
  uv_mutex_destroy(&barrier->mutex);
  return err;

}


void uv_barrier_destroy(uv_barrier_t* barrier) {
  uv_sem_destroy(&barrier->turnstile2);
  uv_sem_destroy(&barrier->turnstile1);
  uv_mutex_destroy(&barrier->mutex);
}


int uv_barrier_wait(uv_barrier_t* barrier) {
  int serial_thread;

  uv_mutex_lock(&barrier->mutex);
  if (++barrier->count == barrier->n) {
    uv_sem_wait(&barrier->turnstile2);
    uv_sem_post(&barrier->turnstile1);
  }
  uv_mutex_unlock(&barrier->mutex);

  uv_sem_wait(&barrier->turnstile1);
  uv_sem_post(&barrier->turnstile1);

  uv_mutex_lock(&barrier->mutex);
  serial_thread = (--barrier->count == 0);
  if (serial_thread) {
    uv_sem_wait(&barrier->turnstile1);
    uv_sem_post(&barrier->turnstile2);
  }
  uv_mutex_unlock(&barrier->mutex);

  uv_sem_wait(&barrier->turnstile2);
  uv_sem_post(&barrier->turnstile2);
  return serial_thread;
}

static void init_once(void) {
	if (uv_mutex_init(&mutex))
		abort();
	QUEUE_INIT(&dtor_queue);
}

int uv_key_create(uv_key_t* key, key_dtor_cb dtor) {
  uv_once(&once, init_once);
  key->tls_index = TlsAlloc();
  QUEUE_INIT(&key->dtor_queue);
  if (key->tls_index == TLS_OUT_OF_INDEXES)
    return UV_ENOMEM;
  key->dtor_cb = dtor;
  uv_mutex_lock(&mutex);
  QUEUE_INSERT_TAIL(&dtor_queue, &key->dtor_queue);
  uv_mutex_unlock(&mutex);
  return 0;
}


void uv_key_delete(uv_key_t* key) {
  if (TlsFree(key->tls_index) == FALSE)
    abort();
  key->tls_index = TLS_OUT_OF_INDEXES;
  uv_mutex_lock(&mutex);
  QUEUE_REMOVE(&key->dtor_queue);
  uv_mutex_unlock(&mutex);
}


void* uv_key_get(uv_key_t* key) {
  void* value;

  value = TlsGetValue(key->tls_index);
  if (value == NULL)
    if (GetLastError() != ERROR_SUCCESS)
      abort();

  return value;
}


void uv_key_set(uv_key_t* key, void* value) {
  if (TlsSetValue(key->tls_index, value) == FALSE)
    abort();
}

// Copyright 2014 The Chromium Authors. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//    * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//    * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//    * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Thread Termination Callbacks.
// Windows doesn't support a per-thread destructor with its
// TLS primitives.  So, we build it manually by inserting a
// function to be called on each thread's exit.
// This magic is from http://www.codeproject.com/threads/tls.asp
// and it works for VC++ 7.0 and later.

// Force a reference to _tls_used to make the linker create the TLS directory
// if it's not already there.  (e.g. if __declspec(thread) is not used).
// Force a reference to p_thread_callback_base to prevent whole program
// optimization from discarding the variable.
#ifdef _WIN64

#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:p_thread_callback_base")

#else  // _WIN64

#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_p_thread_callback_base")

#endif  // _WIN64

// Static callback function to call with each thread termination.
static void NTAPI uv_on_thread_exit(PVOID module, DWORD reason, PVOID reserved) {
	// On XP SP0 & SP1, the DLL_PROCESS_ATTACH is never seen. It is sent on SP2+
	// and on W2K and W2K3. So don't assume it is sent.
	if (DLL_THREAD_DETACH == reason || DLL_PROCESS_DETACH == reason) {
		// call the callbacks
		uv_mutex_lock(&mutex);
		QUEUE* q;
		QUEUE_FOREACH(q, &dtor_queue) {
			uv_key_t* key = QUEUE_DATA(q, uv_key_t, dtor_queue);
			if (key->dtor_cb != NULL)
				key->dtor_cb(uv_key_get(key));
		}
		uv_mutex_unlock(&mutex);
	}
		
}

// .CRT$XLA to .CRT$XLZ is an array of PIMAGE_TLS_CALLBACK pointers that are
// called automatically by the OS loader code (not the CRT) when the module is
// loaded and on thread creation. They are NOT called if the module has been
// loaded by a LoadLibrary() call. It must have implicitly been loaded at
// process startup.
// By implicitly loaded, I mean that it is directly referenced by the main EXE
// or by one of its dependent DLLs. Delay-loaded DLL doesn't count as being
// implicitly loaded.
//
// See VC\crt\src\tlssup.c for reference.

// extern "C" suppresses C++ name mangling so we know the symbol name for the
// linker /INCLUDE:symbol pragma above.
extern "C" {
	// The linker must not discard p_thread_callback_base.  (We force a reference
	// to this variable with a linker /INCLUDE:symbol pragma to ensure that.) If
	// this variable is discarded, the OnThreadExit function will never be called.
#ifdef _WIN64

	// .CRT section is merged with .rdata on x64 so it must be constant data.
#pragma const_seg(".CRT$XLB")
	// When defining a const variable, it must have external linkage to be sure the
	// linker doesn't discard it.
	extern const PIMAGE_TLS_CALLBACK p_thread_callback_base;
	const PIMAGE_TLS_CALLBACK p_thread_callback_base = uv_on_thread_exit;

	// Reset the default section.
#pragma const_seg()

#else  // _WIN64

#pragma data_seg(".CRT$XLB")
	PIMAGE_TLS_CALLBACK p_thread_callback_base = uv_on_thread_exit;

	// Reset the default section.
#pragma data_seg()

#endif  // _WIN64
}