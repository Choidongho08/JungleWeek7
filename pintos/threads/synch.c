/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

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

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
bool cond_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED);
bool donate_priority_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED);
bool nest_donation (struct thread *t);


void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);		// 초기화할 세마포어 포인터가 유효한 메모리 주소인지 검사

	sema->value = value;		// 세마포어가 관리할 자원 갯수 설정
	list_init (&sema->waiters);	// waiterfs 초기화
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. */
void
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable ();// 인터럽트 비활성화

	while (sema->value <= 0) {	// 가용가능한 공유자원이 없는 경우

		list_insert_ordered(&sema->waiters, &thread_current()->elem, priority_sort, NULL); // waiters에 넣기(우선순위 순서대로)

		thread_block ();		// 스레드 block 
	}

	sema->value--;				// 공유자원을 하나 차지했음으로 -1

	intr_set_level (old_level); // 인터럽트 복원
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
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

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);

	old_level = intr_disable ();		// 인터럽트 비활성화
	if (!list_empty (&sema->waiters)){
		list_sort(&sema->waiters,priority_sort,NULL);
		// sema->waiters에 있는 스레드가 있다면 맨 앞의 스레드를 꺼내 thread_unblock()을 호출하여 ready_list로 올리기
		thread_unblock (list_entry (list_pop_front (&sema->waiters), struct thread, elem));
	}

	sema->value++;						// 공유 자원을 다 썼으니 +1 (이제 다른 스레드가 사용 가능)

	intr_set_level (old_level);			// 인터럽트 복원

	thread_preemption();			// waiters에 있던 스레드가 현재 실행중인 스레드의 우선순위보다 높을 수 있음 

}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
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

/* Thread function used by sema_self_test(). */
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

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock));

	struct thread *curr = thread_current();
	
	// 이미 락을 소유한 스레드가 존재하고, 소유자의 우선순위가 요청자(현재 스레드)보다 낮다면
    // 소유자가 작업을 빨리 끝내고 락을 해제할 수 있도록 현재 스레드의 우선순위를 기부한다
	if(lock->holder != NULL){
		curr->lock_to_stop = lock; // 어떤 lock때문에 멈춰 있는지 기록	

		// donation_list에 추가
		list_insert_ordered (&lock->holder->donation_list, &curr->donation_elem, &donate_priority_sort, NULL);	

		if (lock->holder->priority < curr->priority){	// lock 되어있는 스레드보다 우선순위가 높다면
			// donation_list에서 가장 우선순위가 높은 값(가장 앞의 값)을 lock->holder->priority로 저장
			lock->holder->priority = list_entry (list_begin (&lock->holder->donation_list), struct thread, donation_elem)->priority;
		}

		// 중첩 기부 해결 함수
		nest_donation(curr);
	}

	// 락이 풀릴 때까지 뒤에 온(락 건 공유자원을 필요로 한) 스레드들을 재우고 줄세우기 위해
	sema_down (&lock->semaphore);
	// 락 획득
	curr->lock_to_stop = NULL;
    lock->holder = curr;
}

bool nest_donation (struct thread *t){
	// 기다리는 lock이 없을 때나 락을 한 스레드가 없을 경우 return
	if (t == NULL || t->lock_to_stop == NULL || t->lock_to_stop->holder == NULL) return false;

	if(t->lock_to_stop->holder->priority < t->priority){
		t->lock_to_stop->holder->priority = t->priority;
		list_sort(&t->lock_to_stop->holder->donation_list, donate_priority_sort, NULL);
	}

	return nest_donation(t->lock_to_stop->holder);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
// 락 획득 시도
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

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock));

	struct thread *curr = thread_current();


	// 해제한 lock을 기다리던 스레드들을 donation에서 제거
	struct list_elem *e = list_begin (&curr->donation_list);

	while (e != list_end (&curr->donation_list)) {
    	struct thread *t = list_entry (e, struct thread, donation_elem);

    	if (t->lock_to_stop == lock) {
        	/* 지우면서 동시에 다음 노드 주소를 받아서 e에 넣어줌! */
        	e = list_remove (e);
    	} else {
        	e = list_next (e);
    	}
	}

	curr->priority = curr->my_priority;

	if(!list_empty (&curr->donation_list)){	
		// 남은 기부자 중 최대값으로 priority 재계산 후 내 원래 우선순위보다 높다면 교체 진행
		int highest_donation_priority = list_entry (list_begin (&curr->donation_list), struct thread, donation_elem)->priority;

		if(curr->priority < highest_donation_priority) curr->priority = highest_donation_priority;
	}


	lock->holder = NULL;			// unlock
	sema_up (&lock->semaphore);		// 다른 스레드가 unlock한 자원을 사용가능하게함
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
// lock을 하고 있는 스레드가 현재 실행중인 스레드인지를 확인
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	sema_init (&waiter.semaphore, 0);

	list_push_back (&cond->waiters, &waiter.elem);

	lock_release (lock);

	sema_down (&waiter.semaphore);

	lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));

	if (!list_empty (&cond->waiters))
		// cond->waiters 정렬 필요
		list_sort(&cond->waiters,cond_sort,NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters),
					struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

bool cond_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED){

	// 이미 알고 있는 elem의 주소(a)를 바탕으로
	// elem을 품고 있는 semaphore_elem 구조체 본체의 맨 앞 시작 주소를 역산해서 sa에 저장
	struct semaphore_elem *sa = list_entry (a, struct semaphore_elem, elem);
    struct semaphore_elem *sb = list_entry (b, struct semaphore_elem, elem);

	// 각 세마포어(sa, sb) 대기열에서 각각 맨 앞 스레드 1개씩(총 2개) 스레드를 가져옴
    struct thread *ta = list_entry (list_begin (&sa->semaphore.waiters), struct thread, elem);
    struct thread *tb = list_entry (list_begin (&sb->semaphore.waiters), struct thread, elem);

	// 앞 스레드가 우선순위가 높다면 그대로 두기
	if(ta->priority > tb->priority) return true;

	// 뒤 스레드가 우선순위가 높다면 바꿔주기
	else return false;

}

bool donate_priority_sort(const struct list_elem *a,const struct list_elem *b, void *aux UNUSED)
{
	struct thread *t_a = list_entry (a, struct thread, donation_elem);
	struct thread *t_b = list_entry (b, struct thread, donation_elem);

	if (t_a->priority > t_b->priority) return true;
	return false;
}