#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 magic 멤버에 넣는 임의의 값이다. 스택 오버플로 감지에 사용한다. 자세한 내용은 thread.h 상단 설명을 참고한다. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본 스레드에 사용하는 임의의 값이다. 수정하지 않는다. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태, 즉 실행 준비가 되었지만 아직 실행 중은 아닌 스레드들의 목록. */
static struct list ready_list;

/* 유휴 스레드. */
static struct thread *idle_thread;

/* init.c의 main()을 실행하는 최초 스레드. */
static struct thread *initial_thread;

/* allocate_tid()에서 사용하는 잠금. */
static struct lock tid_lock;

/* 스레드 해제 요청 목록. */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* 유휴 상태에서 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서 보낸 타이머 틱 수. */
static long long user_ticks;    /* 사용자 프로그램에서 보낸 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 할당할 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막으로 CPU를 양보한 뒤 경과한 타이머 틱 수. */

/* false(기본값)이면 라운드 로빈 스케줄러, true이면 다단계 피드백 큐 스케줄러를 사용한다. 커널 옵션 -o mlfqs로 설정한다. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것처럼 보이면 true를 반환한다. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* 현재 실행 중인 스레드를 반환한다. CPU 스택 포인터 rsp를 읽어 페이지 시작 주소까지 내림한다. struct thread가 페이지 맨 앞에 있고 스택 포인터는 페이지 중간에 있으므로 이 방식으로 현재 스레드를 찾는다. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start에서 사용할 전역 디스크립터 테이블.
// gdt는 thread_init 이후에 설정된다.
// 따라서 임시 gdt를 먼저 설정한다.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

bool list_if(const struct list_elem *a, const struct list_elem *b, void *aux)
{
	struct thread* a_entry = list_entry(a, struct thread, elem);
	struct thread* b_entry = list_entry(b, struct thread, elem);

	return a_entry->priority > b_entry->priority;
}

/* 현재 실행 중인 코드를 스레드로 바꿔 스레드 시스템을 초기화한다. 일반적으로는 불가능하지만 loader.S가 스택 바닥을 페이지 경계에 두었기 때문에 여기서는 가능하다. 실행 큐와 tid 잠금도 초기화한다. thread_create()로 스레드를 만들기 전에 페이지 할당기를 초기화해야 한다. 이 함수가 끝나기 전에는 thread_current()를 안전하게 호출할 수 없다. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널용 임시 gdt를 다시 불러온다. 이 gdt에는 사용자 컨텍스트가 없다. 커널은 gdt_init()에서 사용자 컨텍스트가 포함된 gdt를 다시 만든다. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트를 초기화한다. */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);

	/* 현재 실행 중인 스레드의 구조체를 설정한다. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* 인터럽트를 활성화해 선점형 스레드 스케줄링을 시작한다. 유휴 스레드도 생성한다. */
void
thread_start (void) {
	/* 유휴 스레드를 생성한다. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* 선점형 스레드 스케줄링을 시작한다. */
	intr_enable ();

	/* 유휴 스레드가 idle_thread를 초기화할 때까지 기다린다. */
	sema_down (&idle_started);
}

/* 타이머 인터럽트 핸들러가 타이머 틱마다 호출하므로, 외부 인터럽트 컨텍스트에서 실행된다. */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계를 갱신한다. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* 선점을 수행한다. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* 스레드 통계를 출력한다. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* 초기 우선순위 PRIORITY를 갖는 NAME 스레드를 만들고, 인자 AUX를 전달해 FUNCTION을 실행하도록 준비한 뒤 실행 큐에 넣는다. 새 스레드의 tid를 반환하며 실패하면 TID_ERROR를 반환한다. thread_start() 이후에는 thread_create()가 반환하기 전에 새 스레드가 실행되거나 종료될 수도 있다. 반대로 새 스레드가 실행될 때까지 기존 스레드가 얼마든지 더 실행될 수 있다. 순서를 보장하려면 세마포어 등의 동기화 수단을 사용한다. 이 코드는 priority 멤버만 설정하며 실제 우선순위 스케줄링은 구현하지 않는다. 우선순위 스케줄링은 과제 1-3의 목표다. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드 메모리를 할당한다. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드를 초기화한다. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스레드가 스케줄되면 kernel_thread를 호출하도록 설정한다. rdi는 첫 번째 인자, rsi는 두 번째 인자다. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* 실행 큐에 추가한다. */
	thread_unblock (t);

	return tid;
}

/* 현재 스레드를 잠들게 한다. thread_unblock()으로 깨어날 때까지 다시 스케줄되지 않는다. 인터럽트를 끈 상태에서 호출해야 한다. 보통 synch.h의 동기화 기능을 사용하는 편이 좋다. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* 차단된 스레드 T를 실행 준비 상태로 바꾼다. T가 차단 상태가 아니면 오류다. 실행 중인 스레드를 준비 상태로 바꾸려면 thread_yield()를 사용한다. 이 함수는 현재 실행 중인 스레드를 선점하지 않는다. 호출자가 인터럽트를 꺼 둔 상태에서 스레드를 깨우고 다른 데이터를 갱신하는 작업을 원자적으로 수행할 수 있어 중요하다. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_push_back (&ready_list, &t->elem);
	list_sort(&ready_list, &list_if, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* 현재 실행 중인 스레드의 이름을 반환한다. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* 현재 실행 중인 스레드를 반환한다. running_thread()에 몇 가지 유효성 검사를 더한 것이다. 자세한 내용은 thread.h 상단 설명을 참고한다. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 실제 스레드인지 확인한다. 검사에 실패하면 스택 오버플로가 발생했을 수 있다. 스레드의 스택은 4KB 미만이므로 큰 지역 배열이나 깊은 재귀로 스택이 넘칠 수 있다. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* 현재 실행 중인 스레드의 tid를 반환한다. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* 현재 스레드의 실행을 중단하고 제거한다. 호출자에게 반환하지 않는다. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 현재 스레드 상태를 종료 중으로 바꾸고 다른 스레드를 스케줄한다. 현재 스레드는 schedule_tail()에서 제거된다. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* CPU를 양보한다. 현재 스레드는 잠들지 않으며 스케줄러 판단에 따라 곧바로 다시 실행될 수도 있다. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
	{
		list_push_back (&ready_list, &curr->elem);
		list_sort(&ready_list, &list_if, NULL);
	}
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* 현재 스레드의 우선순위를 NEW_PRIORITY로 설정한다. */
void
thread_set_priority (int new_priority) {
	thread_current ()->priority = new_priority;
}

/* 현재 스레드의 우선순위를 반환한다. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정한다. */
void
thread_set_nice (int nice UNUSED) {
	/* 구현할 부분. */
}

/* 현재 스레드의 nice 값을 반환한다. */
int
thread_get_nice (void) {
	/* 구현할 부분. */
	return 0;
}

/* 시스템 load average의 100배를 반환한다. */
int
thread_get_load_avg (void) {
	/* 구현할 부분. */
	return 0;
}

/* 현재 스레드 recent_cpu 값의 100배를 반환한다. */
int
thread_get_recent_cpu (void) {
	/* 구현할 부분. */
	return 0;
}

/* 실행 준비된 스레드가 없을 때 실행되는 유휴 스레드다. thread_start()가 처음 실행할 때 유휴 스레드를 실행 큐에 넣는다. 처음 스케줄되면 idle_thread를 초기화하고 thread_start()가 계속되도록 전달받은 세마포어를 up한 뒤 즉시 차단된다. 이후 유휴 스레드는 실행 큐에 나타나지 않는다. 실행 큐가 비었을 때 next_thread_to_run()이 특별히 이 스레드를 반환한다. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행되도록 양보한다. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 켜고 다음 인터럽트를 기다린다. sti 명령은 다음 명령이 끝날 때까지 인터럽트를 비활성 상태로 두므로 두 명령은 원자적으로 실행된다. 그렇지 않으면 인터럽트를 다시 켠 뒤 다음 인터럽트를 기다리는 사이에 인터럽트가 처리되어 타이머 틱 하나만큼 시간을 낭비할 수 있다. HLT 및 STI 명령 설명을 참고한다. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 실행 기반이 되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트가 꺼진 상태에서 실행된다. */
	function (aux);       /* 스레드 함수를 실행한다. */
	thread_exit ();       /* function()이 반환하면 스레드를 종료한다. */
}


/* T를 NAME이라는 차단된 스레드로 기본 초기화한다. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;
}

/* 다음에 스케줄할 스레드를 골라 반환한다. 실행 큐에서 선택하고, 큐가 비면 idle_thread를 반환한다. 현재 스레드가 계속 실행될 수 있다면 실행 큐에 들어 있다. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq로 스레드를 시작한다. */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 페이지 테이블을 활성화하고, 이전 스레드가 종료 중이면 제거하면서 전환한다. 이 함수에 들어올 때 이전 스레드에서 전환이 막 끝났고 새 스레드는 이미 실행 중이며 인터럽트는 꺼져 있다. 전환이 완료되기 전에는 printf()를 호출하면 안 되므로 출력은 함수 끝에 둔다. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 스레드 전환의 핵심 로직이다. intr_frame에 전체 실행 컨텍스트를 복원한 다음 do_iret를 호출해 다음 스레드로 전환한다. 전환이 끝날 때까지 스택을 사용하면 안 된다. */
	__asm __volatile (
			/* 사용할 레지스터를 저장한다. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력값을 한 번 가져온다. */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rcx.
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rbx.
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장된 rax.
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip를 읽는다.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip.
			"movw %%cs, 8(%%rax)\n"  // cs.
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags.
			"mov %%rsp, 24(%%rax)\n" // rsp.
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 스레드를 스케줄한다. 진입 시 인터럽트가 꺼져 있어야 한다. 현재 스레드 상태를 status로 바꾸고 다음 스레드를 찾아 전환한다. schedule() 안에서는 printf()를 호출하면 안 된다. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 현재 스레드를 실행 중으로 표시한다. */
	next->status = THREAD_RUNNING;

	/* 새 시간 할당량을 시작한다. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간을 활성화한다. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환 전 스레드가 종료 중이면 그 스레드 구조체를 제거한다. thread_exit()이 자기 실행 기반을 없애지 않도록 이 작업은 늦게 해야 한다. 페이지가 스택으로 사용 중이므로 여기서는 해제 요청만 큐에 넣고, 실제 해제는 다음 schedule() 시작 때 처리한다. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
			list_sort(&ready_list, &list_if, NULL);
		}

		/* 스레드를 전환하기 전에 현재 실행 중인 스레드의 정보를 저장한다. */
		thread_launch (next);
	}
}

/* 새 스레드에 사용할 tid를 반환한다. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
