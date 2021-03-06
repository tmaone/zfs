/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source. A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2017, 2019 by Delphix. All rights reserved.
 */

/*
 * ZTHR Infrastructure
 * ===================
 *
 * ZTHR threads are used for isolated operations that span multiple txgs
 * within a SPA. They generally exist from SPA creation/loading and until
 * the SPA is exported/destroyed. The ideal requirements for an operation
 * to be modeled with a zthr are the following:
 *
 * 1] The operation needs to run over multiple txgs.
 * 2] There is be a single point of reference in memory or on disk that
 *    indicates whether the operation should run/is running or has
 *    stopped.
 *
 * If the operation satisfies the above then the following rules guarantee
 * a certain level of correctness:
 *
 * 1] Any thread EXCEPT the zthr changes the work indicator from stopped
 *    to running but not the opposite.
 * 2] Only the zthr can change the work indicator from running to stopped
 *    (e.g. when it is done) but not the opposite.
 *
 * This way a normal zthr cycle should go like this:
 *
 * 1] An external thread changes the work indicator from stopped to
 *    running and wakes up the zthr.
 * 2] The zthr wakes up, checks the indicator and starts working.
 * 3] When the zthr is done, it changes the indicator to stopped, allowing
 *    a new cycle to start.
 *
 * Besides being awakened by other threads, a zthr can be configured
 * during creation to wakeup on its own after a specified interval
 * [see zthr_create_timer()].
 *
 * Note: ZTHR threads are NOT a replacement for generic threads! Please
 * ensure that they fit your use-case well before using them.
 *
 * == ZTHR creation
 *
 * Every zthr needs three inputs to start running:
 *
 * 1] A user-defined checker function (checkfunc) that decides whether
 *    the zthr should start working or go to sleep. The function should
 *    return TRUE when the zthr needs to work or FALSE to let it sleep,
 *    and should adhere to the following signature:
 *    boolean_t checkfunc_name(void *args, zthr_t *t);
 *
 * 2] A user-defined ZTHR function (func) which the zthr executes when
 *    it is not sleeping. The function should adhere to the following
 *    signature type:
 *    void func_name(void *args, zthr_t *t);
 *
 * 3] A void args pointer that will be passed to checkfunc and func
 *    implicitly by the infrastructure.
 *
 * The reason why the above API needs two different functions,
 * instead of one that both checks and does the work, has to do with
 * the zthr's internal state lock (zthr_state_lock) and the allowed
 * cancellation windows. We want to hold the zthr_state_lock while
 * running checkfunc but not while running func. This way the zthr
 * can be cancelled while doing work and not while checking for work.
 *
 * To start a zthr:
 *     zthr_t *zthr_pointer = zthr_create(checkfunc, func, args);
 *
 * After that you should be able to wakeup, cancel, and resume the
 * zthr from another thread using the zthr_pointer.
 *
 * NOTE: ZTHR threads could potentially wake up spuriously and the
 * user should take this into account when writing a checkfunc.
 * [see ZTHR state transitions]
 *
 * == ZTHR cancellation
 *
 * ZTHR threads must be cancelled when their SPA is being exported
 * or when they need to be paused so they don't interfere with other
 * operations.
 *
 * To cancel a zthr:
 *     zthr_cancel(zthr_pointer);
 *
 * To resume it:
 *     zthr_resume(zthr_pointer);
 *
 * A zthr will implicitly check if it has received a cancellation
 * signal every time func returns and every time it wakes up [see
 * ZTHR state transitions below].
 *
 * At times, waiting for the zthr's func to finish its job may take
 * time. This may be very time-consuming for some operations that
 * need to cancel the SPA's zthrs (e.g spa_export). For this scenario
 * the user can explicitly make their ZTHR function aware of incoming
 * cancellation signals using zthr_iscancelled(). A common pattern for
 * that looks like this:
 *
 * int
 * func_name(void *args, zthr_t *t)
 * {
 *     ... <unpack args> ...
 *     while (!work_done && !zthr_iscancelled(t)) {
 *         ... <do more work> ...
 *     }
 * }
 *
 * == ZTHR cleanup
 *
 * Cancelling a zthr doesn't clean up its metadata (internal locks,
 * function pointers to func and checkfunc, etc..). This is because
 * we want to keep them around in case we want to resume the execution
 * of the zthr later. Similarly for zthrs that exit themselves.
 *
 * To completely cleanup a zthr, cancel it first to ensure that it
 * is not running and then use zthr_destroy().
 *
 * == ZTHR state transitions
 *
 *    zthr creation
 *      +
 *      |
 *      |      woke up
 *      |   +--------------+ sleep
 *      |   |                  ^
 *      |   |                  |
 *      |   |                  | FALSE
 *      |   |                  |
 *      v   v     FALSE        +
 *   cancelled? +---------> checkfunc?
 *      +   ^                  +
 *      |   |                  |
 *      |   |                  | TRUE
 *      |   |                  |
 *      |   |  func returned   v
 *      |   +---------------+ func
 *      |
 *      | TRUE
 *      |
 *      v
 *   zthr stopped running
 *
 * == Implementation of ZTHR requests
 *
 * ZTHR wakeup, cancel, and resume are requests on a zthr to
 * change its internal state. Requests on a zthr are serialized
 * using the zthr_request_lock, while changes in its internal
 * state are protected by the zthr_state_lock. A request will
 * first acquire the zthr_request_lock and then immediately
 * acquire the zthr_state_lock. We do this so that incoming
 * requests are serialized using the request lock, while still
 * allowing us to use the state lock for thread communication
 * via zthr_cv.
 */

#include <sys/zfs_context.h>
#include <sys/zthr.h>

struct zthr {
	/* running thread doing the work */
	kthread_t	*zthr_thread;

	/* lock protecting internal data & invariants */
	kmutex_t	zthr_state_lock;

	/* mutex that serializes external requests */
	kmutex_t	zthr_request_lock;

	/* notification mechanism for requests */
	kcondvar_t	zthr_cv;

	/* flag set to true if we are canceling the zthr */
	boolean_t	zthr_cancel;

	/*
	 * maximum amount of time that the zthr is spent sleeping;
	 * if this is 0, the thread doesn't wake up until it gets
	 * signaled.
	 */
	hrtime_t	zthr_wait_time;

	/* consumer-provided callbacks & data */
	zthr_checkfunc_t	*zthr_checkfunc;
	zthr_func_t	*zthr_func;
	void		*zthr_arg;
};

static void
zthr_procedure(void *arg)
{
	zthr_t *t = arg;

	mutex_enter(&t->zthr_state_lock);
	ASSERT3P(t->zthr_thread, ==, curthread);

	while (!t->zthr_cancel) {
		if (t->zthr_checkfunc(t->zthr_arg, t)) {
			mutex_exit(&t->zthr_state_lock);
			t->zthr_func(t->zthr_arg, t);
			mutex_enter(&t->zthr_state_lock);
		} else {
			/* go to sleep */
			if (t->zthr_wait_time == 0) {
				cv_wait_sig(&t->zthr_cv, &t->zthr_state_lock);
			} else {
				(void) cv_timedwait_hires(&t->zthr_cv,
				    &t->zthr_state_lock, t->zthr_wait_time,
				    MSEC2NSEC(1), 0);
			}
		}
	}

	/*
	 * Clear out the kernel thread metadata and notify the
	 * zthr_cancel() thread that we've stopped running.
	 */
	t->zthr_thread = NULL;
	t->zthr_cancel = B_FALSE;
	cv_broadcast(&t->zthr_cv);

	mutex_exit(&t->zthr_state_lock);
	thread_exit();
}

zthr_t *
zthr_create(zthr_checkfunc_t *checkfunc, zthr_func_t *func, void *arg)
{
	zthr_t *t = kmem_zalloc(sizeof (*t), KM_SLEEP);
	mutex_init(&t->zthr_state_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&t->zthr_request_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&t->zthr_cv, NULL, CV_DEFAULT, NULL);

	mutex_enter(&t->zthr_state_lock);
	t->zthr_checkfunc = checkfunc;
	t->zthr_func = func;
	t->zthr_arg = arg;

	t->zthr_thread = thread_create(NULL, 0, zthr_procedure, t,
	    0, &p0, TS_RUN, minclsyspri);
	mutex_exit(&t->zthr_state_lock);

	return (t);
}

void
zthr_destroy(zthr_t *t)
{
	ASSERT(!MUTEX_HELD(&t->zthr_state_lock));
	ASSERT(!MUTEX_HELD(&t->zthr_request_lock));
	VERIFY3P(t->zthr_thread, ==, NULL);
	mutex_destroy(&t->zthr_request_lock);
	mutex_destroy(&t->zthr_state_lock);
	cv_destroy(&t->zthr_cv);
	kmem_free(t, sizeof (*t));
}

/*
 * Wake up the zthr if it is sleeping. If the thread has been
 * cancelled that does nothing.
 */
void
zthr_wakeup(zthr_t *t)
{
	mutex_enter(&t->zthr_request_lock);
	mutex_enter(&t->zthr_state_lock);

	/*
	 * There are 4 states that we can find the zthr when issuing
	 * this broadcast:
	 *
	 * [1] The common case of the thread being asleep, at which
	 *     point the broadcast will wake it up.
	 * [2] The thread has been cancelled. Waking up a cancelled
	 *     thread is a no-op. Any work that is still left to be
	 *     done should be handled the next time the thread is
	 *     resumed.
	 * [3] The thread is doing work and is already up, so this
	 *     is basically a no-op.
	 * [4] The thread was just created/resumed, in which case the
	 *     behavior is similar to [3].
	 */
	cv_broadcast(&t->zthr_cv);

	mutex_exit(&t->zthr_state_lock);
	mutex_exit(&t->zthr_request_lock);
}

/*
 * Sends a cancel request to the zthr and blocks until the zthr is
 * cancelled. If the zthr is not running (e.g. has been cancelled
 * already), this is a no-op.
 */
void
zthr_cancel(zthr_t *t)
{
	mutex_enter(&t->zthr_request_lock);
	mutex_enter(&t->zthr_state_lock);

	/*
	 * Since we are holding the zthr_state_lock at this point
	 * we can find the state in one of the following 4 states:
	 *
	 * [1] The thread has already been cancelled, therefore
	 *     there is nothing for us to do.
	 * [2] The thread is sleeping, so we broadcast the CV first
	 *     to wake it up and then we set the flag and we are
	 *     waiting for it to exit.
	 * [3] The thread is doing work, in which case we just set
	 *     the flag and wait for it to finish.
	 * [4] The thread was just created/resumed, in which case
	 *     the behavior is similar to [3].
	 *
	 * Since requests are serialized, by the time that we get
	 * control back we expect that the zthr is cancelled and
	 * not running anymore.
	 */
	if (t->zthr_thread != NULL) {
		t->zthr_cancel = B_TRUE;

		/* broadcast in case the zthr is sleeping */
		cv_broadcast(&t->zthr_cv);

		while (t->zthr_thread != NULL)
			cv_wait(&t->zthr_cv, &t->zthr_state_lock);

		ASSERT(!t->zthr_cancel);
	}

	mutex_exit(&t->zthr_state_lock);
	mutex_exit(&t->zthr_request_lock);
}

/*
 * Sends a resume request to the supplied zthr. If the zthr is
 * already running this is a no-op.
 */
void
zthr_resume(zthr_t *t)
{
	mutex_enter(&t->zthr_request_lock);
	mutex_enter(&t->zthr_state_lock);

	ASSERT3P(&t->zthr_checkfunc, !=, NULL);
	ASSERT3P(&t->zthr_func, !=, NULL);
	ASSERT(!t->zthr_cancel);

	/*
	 * There are 4 states that we find the zthr in at this point
	 * given the locks that we hold:
	 *
	 * [1] The zthr was cancelled, so we spawn a new thread for
	 *     the zthr (common case).
	 * [2] The zthr is running at which point this is a no-op.
	 * [3] The zthr is sleeping at which point this is a no-op.
	 * [4] The zthr was just spawned at which point this is a
	 *     no-op.
	 */
	if (t->zthr_thread == NULL) {
		t->zthr_thread = thread_create(NULL, 0, zthr_procedure, t,
		    0, &p0, TS_RUN, minclsyspri);
	}

	mutex_exit(&t->zthr_state_lock);
	mutex_exit(&t->zthr_request_lock);
}

/*
 * This function is intended to be used by the zthr itself
 * (specifically the zthr_func callback provided) to check
 * if another thread has signaled it to stop running before
 * doing some expensive operation.
 *
 * returns TRUE if we are in the middle of trying to cancel
 *     this thread.
 *
 * returns FALSE otherwise.
 */
boolean_t
zthr_iscancelled(zthr_t *t)
{
	ASSERT3P(t->zthr_thread, ==, curthread);

	/*
	 * The majority of the functions here grab zthr_request_lock
	 * first and then zthr_state_lock. This function only grabs
	 * the zthr_state_lock. That is because this function should
	 * only be called from the zthr_func to check if someone has
	 * issued a zthr_cancel() on the thread. If there is a zthr_cancel()
	 * happening concurrently, attempting to grab the request lock
	 * here would result in a deadlock.
	 *
	 * By grabbing only the zthr_state_lock this function is allowed
	 * to run concurrently with a zthr_cancel() request.
	 */
	mutex_enter(&t->zthr_state_lock);
	boolean_t cancelled = t->zthr_cancel;
	mutex_exit(&t->zthr_state_lock);
	return (cancelled);
}
