/* 이 파일은 Nachos 교육용 운영체제의 소스 코드를 바탕으로 작성되었다. Nachos의 저작권 고지는 아래에 전문을 수록했다. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* 세마포어 SEMA를 VALUE 값으로 초기화한다. 세마포어는 음이 아닌 정수이며, 값을 조작하는 두 가지 원자적 연산을 제공한다:
- down 또는 P: 값이 양수가 될 때까지 기다린 다음 1 감소시킨다.
- up 또는 V: 값을 1 증가시키고, 기다리는 스레드가 있으면 하나를 깨운다. */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* 세마포어의 down 또는 P 연산이다. SEMA 값이 양수가 될 때까지 기다린 뒤 원자적으로 1 감소시킨다. 이 함수는 잠들 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다. 인터럽트가 꺼진 상태에서 호출할 수 있지만, 잠들면 다음에 스케줄된 스레드가 인터럽트를 다시 켤 가능성이 높다. 이 함수는 sema_down이다. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();
	while (sema->value == 0) {
		list_push_back (&sema->waiters, &thread_current()->elem);
		thread_block ();
	}
	sema->value--;
	intr_set_level (old_level);
}

/* 세마포어 값이 0이 아닐 때만 down 또는 P 연산을 수행한다. 세마포어 값을 감소시키면 true, 그렇지 않으면 false를 반환한다. 인터럽트 핸들러에서도 호출할 수 있다. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

bool priority_bigger (const struct list_elem *a, const struct list_elem *b, void *aux)
{
	const int a_pri = list_entry(a, struct thread, elem)->priority;
	const int b_pri = list_entry(b, struct thread, elem)->priority;

	return a_pri < b_pri;
}

/* 세마포어의 up 또는 V 연산이다. SEMA 값을 1 증가시키고, 기다리는 스레드가 있으면 하나를 깨운다. 인터럽트 핸들러에서도 호출할 수 있다. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (!list_empty (&sema->waiters))
	{
		struct list_elem* list_elem = list_max(&sema->waiters, &priority_bigger, NULL);
		list_remove(list_elem);
		thread_unblock(list_entry(list_elem, struct thread, elem));
	}
	sema->value++;
	intr_set_level (old_level);
	priority_thread_yield();
}

static void sema_test_helper (void *sema_);

/* 두 스레드 사이에서 실행 흐름을 핑퐁처럼 주고받는 세마포어 자체 테스트다. 어떤 일이 일어나는지 보려면 printf() 호출을 추가한다. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* sema_self_test()에서 사용하는 스레드 함수. */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* LOCK을 초기화한다. 한 번에 하나의 스레드만 잠금을 보유할 수 있다. 잠금은 재귀적이지 않으므로, 잠금을 보유한 스레드가 같은 잠금을 다시 얻으려 하면 오류다. 잠금은 초기값이 1인 세마포어의 특수한 형태다. 차이는 두 가지다. 세마포어 값은 1보다 클 수 있지만 잠금은 한 번에 한 스레드만 소유할 수 있다. 또한 세마포어에는 소유자가 없어 한 스레드가 down하고 다른 스레드가 up할 수 있지만, 잠금은 같은 스레드가 획득과 해제를 모두 해야 한다. 이런 제약이 불편하다면 잠금보다 세마포어를 사용하는 편이 적절하다. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* 필요한 경우 잠들며 LOCK을 획득한다. 현재 스레드가 이미 이 잠금을 보유한 상태여서는 안 된다. 이 함수는 잠들 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다. 인터럽트가 꺼진 상태에서 호출할 수 있지만, 잠들어야 하면 인터럽트가 다시 켜진다. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	sema_down (&lock->semaphore);
	lock->holder = thread_current ();
}

/* LOCK 획득을 시도한다. 성공하면 true, 실패하면 false를 반환한다. 현재 스레드가 이미 이 잠금을 보유한 상태여서는 안 된다. 이 함수는 잠들지 않으므로 인터럽트 핸들러 안에서도 호출할 수 있다. */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* 현재 스레드가 소유한 LOCK을 해제한다. lock_release 함수다. 인터럽트 핸들러는 잠금을 획득할 수 없으므로, 인터럽트 핸들러 안에서 잠금을 해제하는 것도 적절하지 않다. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	lock->holder = NULL;
	sema_up (&lock->semaphore);
}

/* 현재 스레드가 LOCK을 보유하고 있으면 true, 아니면 false를 반환한다. 다른 스레드가 잠금을 보유하는지 검사하면 경쟁 상태가 발생할 수 있다. */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* 목록에 들어가는 세마포어 하나. */
struct semaphore_elem {
	struct list_elem elem;              /* 목록 요소. */
	struct semaphore semaphore;         /* 이 세마포어. */
	struct thread* wait_thread;
};

/* 조건 변수 COND를 초기화한다. 조건 변수는 한 코드가 조건을 알리고, 이에 협력하는 다른 코드가 신호를 받아 동작하도록 한다. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

struct  thread* last_cond_thread;

/* LOCK을 원자적으로 해제하고 다른 코드가 COND에 신호를 보낼 때까지 기다린다. 신호를 받은 뒤 반환하기 전에 LOCK을 다시 획득한다. 이 함수를 호출하기 전에 LOCK을 보유하고 있어야 한다. 이 함수가 구현하는 모니터는 Hoare 방식이 아닌 Mesa 방식이다. 즉, 신호를 보내는 것과 받는 것이 원자적이지 않다. 따라서 일반적으로 대기가 끝난 뒤 호출자가 조건을 다시 확인하고, 필요하면 다시 기다려야 한다. 하나의 조건 변수는 하나의 잠금에만 연결되지만, 하나의 잠금은 여러 조건 변수와 연결될 수 있다. 즉 잠금과 조건 변수는 일대다 관계다. 이 함수는 잠들 수 있으므로 인터럽트 핸들러 안에서 호출하면 안 된다. 인터럽트가 꺼진 상태에서 호출할 수 있지만, 잠들어야 하면 인터럽트가 다시 켜진다. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);
	waiter.wait_thread = thread_current();
	list_push_back (&cond->waiters, &waiter.elem);
	lock_release (lock);
	sema_down (&waiter.semaphore);
	lock_acquire (lock);
}

bool sema_priority_bigger (const struct list_elem *a, const struct list_elem *b, void *aux)
{
	const int a_pri = list_entry(a, struct semaphore_elem, elem)->wait_thread->priority;
	const int b_pri = list_entry(b, struct semaphore_elem, elem)->wait_thread->priority;

	return a_pri < b_pri;
}

/* LOCK으로 보호되는 COND를 기다리는 스레드가 있으면 그중 하나에 신호를 보내 대기에서 깨어나게 한다. 이 함수를 호출하기 전에 LOCK을 보유해야 한다. 인터럽트 핸들러는 잠금을 획득할 수 없으므로, 인터럽트 핸들러 안에서 조건 변수에 신호를 보내는 것은 적절하지 않다. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
	{
		struct list_elem* max_elem = list_max(&cond->waiters, &sema_priority_bigger, NULL);
		list_remove(max_elem);
		struct semaphore_elem* sema_elem = list_entry(max_elem, struct semaphore_elem, elem);
		
		sema_up(&sema_elem->semaphore);
	}
}

/* LOCK으로 보호되는 COND를 기다리는 스레드가 있으면 모두 깨운다. 이 함수를 호출하기 전에 LOCK을 보유해야 한다. 인터럽트 핸들러는 잠금을 획득할 수 없으므로, 인터럽트 핸들러 안에서 조건 변수에 신호를 보내는 것은 적절하지 않다. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}
