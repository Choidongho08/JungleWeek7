#include "devices/timer.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <list.h>
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/synch.h"
#include "threads/thread.h"

/* 8254 타이머 칩의 하드웨어 세부 사항은 [8254]를 참고한다. */

#if TIMER_FREQ < 19
#error 8254 timer requires TIMER_FREQ >= 19
#endif
#if TIMER_FREQ > 1000
#error TIMER_FREQ <= 1000 recommended
#endif

struct wake_up_sema
{
	int64_t start_tick;
	int64_t target_tick;
	struct semaphore* sema;
	struct list_elem elem;
};

static struct list wake_up_list;

/* 운영체제 부팅 이후 발생한 타이머 틱 수. */
static int64_t ticks;

/* 타이머 틱당 반복 횟수.
   timer_calibrate()에서 초기화한다. */
static unsigned loops_per_tick;

static intr_handler_func timer_interrupt;
static bool too_many_loops (unsigned loops);
static void busy_wait (int64_t loops);
static void real_time_sleep (int64_t num, int32_t denom);

/* 8254 프로그래머블 인터벌 타이머(PIT)가 초당 PIT_FREQ번
   인터럽트를 발생시키도록 설정하고, 해당 인터럽트를 등록한다. */
void timer_init (void) {
	/* 8254의 입력 주파수를 TIMER_FREQ로 나눈 값을
	   가장 가까운 정수로 반올림한다. */
	uint16_t count = (1193180 + TIMER_FREQ / 2) / TIMER_FREQ;

	outb (0x43, 0x34);    /* 제어 워드(CW): 카운터 0, 하위 바이트 다음 상위 바이트, 모드 2, 이진수. */
	outb (0x40, count & 0xff);
	outb (0x40, count >> 8);

	intr_register_ext (0x20, timer_interrupt, "8254 Timer");

	list_init(&wake_up_list);
}

/* 짧은 지연을 구현하는 데 사용하는 loops_per_tick을 보정한다. */
void timer_calibrate (void) {
	unsigned high_bit, test_bit;

	ASSERT (intr_get_level () == INTR_ON);
	printf ("Calibrating timer...  ");

	/* 실행 시간이 타이머 한 틱보다 짧은 최대 2의 거듭제곱을
	   loops_per_tick의 근삿값으로 구한다. */
	loops_per_tick = 1u << 10; // 1024
	while (!too_many_loops (loops_per_tick << 1)) {
		loops_per_tick <<= 1;
		ASSERT (loops_per_tick != 0);
	}

	/* loops_per_tick의 다음 8비트를 정밀하게 보정한다. */
	high_bit = loops_per_tick;
	for (test_bit = high_bit >> 1; test_bit != high_bit >> 10; test_bit >>= 1)
		if (!too_many_loops (high_bit | test_bit))
			loops_per_tick |= test_bit;

	printf ("%'"PRIu64" loops/s.\n", (uint64_t) loops_per_tick * TIMER_FREQ);
}

/* 운영체제 부팅 이후 발생한 타이머 틱 수를 반환한다. */
int64_t timer_ticks (void) {
	enum intr_level old_level = intr_disable ();
	int64_t t = ticks;
	intr_set_level (old_level);
	barrier ();
	return t;
}

/* THEN 이후 경과한 타이머 틱 수를 반환한다.
   THEN은 이전에 timer_ticks()가 반환한 값이어야 한다. */
int64_t timer_elapsed (int64_t then) {
	return timer_ticks () - then;
}

/* 약 TICKS개의 타이머 틱 동안 실행을 중단한다. */
void timer_sleep (int64_t ticks) {
	
	struct semaphore sleep_sema;
	struct wake_up_sema sema;
	sema.start_tick = timer_ticks();
	sema.target_tick = ticks;
	sema_init(&sleep_sema, 0);
	sema.sema = &sleep_sema;
	enum intr_level old_val = intr_disable();	
	list_push_back(&wake_up_list, &sema.elem);
	intr_set_level(old_val);
	
	ASSERT (intr_get_level () == INTR_ON);
	sema_down(sema.sema);
}

/* 약 MS밀리초 동안 실행을 중단한다. */
void timer_msleep (int64_t ms) {
	real_time_sleep (ms, 1000);
}

/* 약 US마이크로초 동안 실행을 중단한다. */
void timer_usleep (int64_t us) {
	real_time_sleep (us, 1000 * 1000);
}

/* 약 NS나노초 동안 실행을 중단한다. */
void timer_nsleep (int64_t ns) {
	real_time_sleep (ns, 1000 * 1000 * 1000);
}

/* 타이머 통계를 출력한다. */
void timer_print_stats (void) {
	printf ("Timer: %"PRId64" ticks\n", timer_ticks ());
}

/* 타이머 인터럽트 핸들러. */
static void timer_interrupt (struct intr_frame *args UNUSED) {
	ticks++;
	thread_tick ();

	for(struct list_elem* for_elem = list_begin(&wake_up_list); for_elem != list_end(&wake_up_list); for_elem = list_next(for_elem))
	{
		struct wake_up_sema* data = list_entry(for_elem, struct wake_up_sema, elem);
		if(timer_ticks() - data->start_tick >= data->target_tick)
		{
			sema_up(data->sema);

			enum intr_level old_val = intr_disable();
			list_remove(&data->elem);
			intr_set_level(old_val);
		}
	}
}

/* LOOPS번 반복하는 데 걸리는 시간이 타이머 한 틱보다 길면
   true를 반환하고, 그렇지 않으면 false를 반환한다. */
static bool too_many_loops (unsigned loops) {
	/* 타이머 틱이 발생할 때까지 기다린다. */
	int64_t start = ticks;
	while (ticks == start)
		barrier ();

	/* 루프를 LOOPS번 실행한다. */
	start = ticks;
	busy_wait (loops);

	/* 틱 수가 바뀌었다면 반복에 너무 오랜 시간이 걸린 것이다. */
	barrier ();
	return start != ticks;
}

/* 짧은 지연을 구현하기 위해 단순한 루프를 LOOPS번 반복한다.

   코드 정렬은 실행 시간에 큰 영향을 줄 수 있다. 이 함수가 위치마다
   다르게 인라인화되면 결과를 예측하기 어려워지므로
   NO_INLINE으로 지정한다. */
static void NO_INLINE busy_wait (int64_t loops) {
	while (loops-- > 0)
		barrier ();
}

/* 약 NUM/DENOM초 동안 실행을 중단한다. */
static void real_time_sleep (int64_t num, int32_t denom) {
	/* NUM/DENOM초를 타이머 틱 수로 변환하며, 소수점 이하는 버린다.

	   (NUM / DENOM) s
	   ---------------------- = NUM * TIMER_FREQ / DENOM ticks.
	   1 s / TIMER_FREQ ticks
	   */
	int64_t ticks = num * TIMER_FREQ / denom;

	ASSERT (intr_get_level () == INTR_ON);
	if (ticks > 0) {
		/* 최소한 타이머 한 틱 전체에 해당하는 시간 동안 기다린다.
		   다른 프로세스에 CPU를 양보할 수 있도록 timer_sleep()을 사용한다. */
		timer_sleep (ticks);
	} else {
		/* 그렇지 않으면 한 틱보다 짧은 시간을 더 정확하게 맞추기 위해
		   바쁜 대기 루프를 사용한다. 오버플로 가능성을 피하려고
		   분자와 분모를 각각 1000으로 나누어 크기를 줄인다. */
		ASSERT (denom % 1000 == 0);
		busy_wait (loops_per_tick * num / 1000 * TIMER_FREQ / (denom / 1000));
	}
}
