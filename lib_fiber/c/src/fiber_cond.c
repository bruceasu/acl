#include "stdafx.h"
#include "common.h"

#ifdef SYS_UNIX

#include "fiber/libfiber.h"
#include "fiber/fiber_cond.h"
#include "fiber.h"

struct ACL_FIBER_COND {
	RING            waiters;
	ATOMIC         *atomic;
	long long       value;
	pthread_mutex_t mutex;
};

ACL_FIBER_COND *acl_fiber_cond_create(unsigned flag fiber_unused)
{
	pthread_mutexattr_t attr;
	ACL_FIBER_COND *cond = (ACL_FIBER_COND *)
		calloc(1, sizeof(ACL_FIBER_COND));

	ring_init(&cond->waiters);
	cond->atomic = atomic_new();
	atomic_set(cond->atomic, &cond->value);
	atomic_int64_set(cond->atomic, 0);

	pthread_mutexattr_init(&attr);
	pthread_mutex_init(&cond->mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	return cond;
}

void acl_fiber_cond_free(ACL_FIBER_COND *cond)
{
	pthread_mutex_destroy(&cond->mutex);
	atomic_free(cond->atomic);
	free(cond);
}

static void __ll_lock(ACL_FIBER_COND *cond)
{
	int n = pthread_mutex_lock(&cond->mutex);
	if (n) {
		acl_fiber_set_error(n);
		msg_fatal("%s(%d), %s: pthread_mutex_lock error=%s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
}

static void __ll_unlock(ACL_FIBER_COND *cond)
{
	int n = pthread_mutex_unlock(&cond->mutex);
	if (n) {
		acl_fiber_set_error(n);
		msg_fatal("%s(%d), %s: pthread_mutex_unlock error=%s",
			__FILE__, __LINE__, __FUNCTION__, last_serror());
	}
}

#define DETACHE do {                       \
	__ll_lock(cond);                   \
	fbase_event_close(fbase);          \
	ring_detach(&fbase->event_waiter); \
	__ll_unlock(cond);                 \
	if (fbase->flag & FBASE_F_BASE) {  \
		fbase_free(fbase);         \
	}                                  \
} while (0)

int acl_fiber_cond_wait(ACL_FIBER_COND *cond, ACL_FIBER_EVENT *event)
{
	ACL_FIBER  *fiber = acl_fiber_running();
	FIBER_BASE *fbase;

	fbase = fiber ? &fiber->base : fbase_alloc();
	fbase_event_open(fbase);

	__ll_lock(cond);
	ring_prepend(&cond->waiters, &fbase->event_waiter);
	__ll_unlock(cond);

	if (acl_fiber_event_notify(event) != 0) {
		DETACHE;
		msg_fatal("acl_fiber_event_notify failed");
	}

	if (fbase_event_wait(fbase) == -1) {
		DETACHE;
		msg_fatal("fbase_event_wait error");
	}

	if (acl_fiber_event_wait(event) == -1) {
		DETACHE;
		msg_fatal("acl_fiber_event_wait error");
	}

	fbase_event_close(fbase);
	if (fbase->flag & FBASE_F_BASE) {
		fbase_free(fbase);
	}

	return 0;
}

static int read_wait(int fd, int delay)
{
	struct pollfd fds;

	fds.events = POLLIN;
	fds.fd     = fd;

	for (;;) {
		switch (acl_fiber_poll(&fds, 1, delay)) {
#ifdef SYS_WIN
		case SOCKET_ERROR:
#else
		case -1:
#endif
			if (acl_fiber_last_error() == EINTR) {
				continue;
			}
			return -1;
		case 0:
			acl_fiber_set_error(ETIMEDOUT);
			return -1;
		default:
			if ((fds.revents & POLLIN)) {
				return 0;
			}
			if (fds.revents & (POLLHUP | POLLERR | POLLNVAL)) {
				return 0;
			}

			return -1;
		}
	}
}

int acl_fiber_cond_timedwait(ACL_FIBER_COND *cond, ACL_FIBER_EVENT *event,
	int delay_ms)
{
	ACL_FIBER  *fiber = acl_fiber_running();
	FIBER_BASE *fbase;

	fbase = fiber ? &fiber->base : fbase_alloc();
	fbase_event_open(fbase);

	__ll_lock(cond);
	ring_prepend(&cond->waiters, &fbase->event_waiter);
	__ll_unlock(cond);

	if (acl_fiber_event_notify(event) != 0) {
		DETACHE;
		msg_error("acl_fiber_event_notify failed");
		return EINVAL;
	}

	while (1) {
		if (read_wait(fbase->event_in, delay_ms) == -1) {
			if (acl_fiber_event_wait(event) == -1) {
				msg_fatal("%s(%d), %s: wait event error",
					__FILE__, __LINE__, __FUNCTION__);
			}
			DETACHE;
			return ETIMEDOUT;
		}

		__ll_lock(cond);
		if (atomic_int64_cas(cond->atomic, 0, 1) == 0) {
			break;
		}
		__ll_unlock(cond);
	}

	if (fbase_event_wait(fbase) == -1) {
		if (atomic_int64_cas(cond->atomic, 1, 0) != 1) {
			msg_fatal("%s(%d), %s: cond corrupt",
				__FILE__, __LINE__, __FUNCTION__);
		}
		__ll_unlock(cond);

		if (acl_fiber_event_wait(event) == -1) {
			msg_fatal("%s(%d), %s: wait event error",
				__FILE__, __LINE__, __FUNCTION__);
		}
		DETACHE;
		return EINVAL;
	}

	if (atomic_int64_cas(cond->atomic, 1, 0) != 1) {
		msg_fatal("%s(%d), %s: cond corrupt",
			__FILE__, __LINE__, __FUNCTION__);
	}
	__ll_unlock(cond);

	if (acl_fiber_event_wait(event) == -1) {
		DETACHE;
		msg_error("acl_fiber_event_wait error");
		return EINVAL;
	}

	fbase_event_close(fbase);
	if (fbase->flag & FBASE_F_BASE) {
		fbase_free(fbase);
	}

	return 0;
}

int acl_fiber_cond_signal(ACL_FIBER_COND *cond)
{
	FIBER_BASE *waiter;
	RING       *head;

	__ll_lock(cond);

	head = ring_pop_head(&cond->waiters);
	if (head) {
		waiter = RING_TO_APPL(head, FIBER_BASE, event_waiter);
	} else {
		waiter = NULL;
	}

	__ll_unlock(cond);

	if (waiter && fbase_event_wakeup(waiter) == -1) {
		msg_error("fbase_event_wakeup error");
		return EINVAL;
	}

	return 0;
}

#endif // SYS_UNIX
