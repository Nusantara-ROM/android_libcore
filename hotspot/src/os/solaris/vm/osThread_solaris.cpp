/*
 * Copyright (c) 1998, 2010, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// no precompiled headers
#include "runtime/atomic.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/osThread.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/vmThread.hpp"
#ifdef TARGET_ARCH_x86
# include "assembler_x86.inline.hpp"
#endif
#ifdef TARGET_ARCH_sparc
# include "assembler_sparc.inline.hpp"
#endif

# include <signal.h>

 // ***************************************************************
 // Platform dependent initialization and cleanup
 // ***************************************************************

void OSThread::pd_initialize() {
  _thread_id                         = 0;
  sigemptyset(&_caller_sigmask);

  _saved_interrupt_thread_state      = _thread_new;
  _vm_created_thread                 = false;
}

void OSThread::pd_destroy() {
}

// copied from synchronizer.cpp

void OSThread::handle_spinlock_contention(int tries) {
  if (NoYieldsInMicrolock) return;

  if (tries > 10) {
    os::yield_all(tries); // Yield to threads of any priority
  } else if (tries > 5) {
    os::yield();          // Yield to threads of same or higher priority
  }
}

static void resume_clear_context(OSThread *osthread) {
  osthread->set_ucontext(NULL);
}

static void suspend_save_context(OSThread *osthread, ucontext_t* context) {
  osthread->set_ucontext(context);
}

void OSThread::SR_handler(Thread* thread, ucontext_t* uc) {
  // Save and restore errno to avoid confusing native code with EINTR
  // after sigsuspend.
  int old_errno = errno;

  OSThread* osthread = thread->osthread();
  assert(thread->is_VM_thread() || thread->is_Java_thread(), "Must be VMThread or JavaThread");

  os::SuspendResume::State current = osthread->sr.state();
  if (current == os::SuspendResume::SR_SUSPEND_REQUEST) {
    suspend_save_context(osthread, uc);

    // attempt to switch the state, we assume we had a SUSPEND_REQUEST
    os::SuspendResume::State state = osthread->sr.suspended();
    if (state == os::SuspendResume::SR_SUSPENDED) {
      sigset_t suspend_set;  // signals for sigsuspend()

      // get current set of blocked signals and unblock resume signal
      thr_sigsetmask(SIG_BLOCK, NULL, &suspend_set);
      sigdelset(&suspend_set, os::Solaris::SIGasync());

      // wait here until we are resumed
      while (1) {
        sigsuspend(&suspend_set);

        os::SuspendResume::State result = osthread->sr.running();
        if (result == os::SuspendResume::SR_RUNNING) {
          break;
        }
      }

    } else if (state == os::SuspendResume::SR_RUNNING) {
      // request was cancelled, continue
    } else {
      ShouldNotReachHere();
    }

    resume_clear_context(osthread);
  } else if (current == os::SuspendResume::SR_RUNNING) {
    // request was cancelled, continue
  } else if (current == os::SuspendResume::SR_WAKEUP_REQUEST) {
    // ignore
  } else {
    ShouldNotReachHere();
  }

  errno = old_errno;
}
