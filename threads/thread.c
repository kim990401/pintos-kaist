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
#include "userprog/process.h"

// th hi
/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* list of threads in THREAD_BLOCK state. */
static struct list sleep_list;
static struct list all_threads;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);
    load_avg = 0;

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list);
    list_init (&all_threads);

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
    if (thread_mlfqs)
        list_push_back(&all_threads, &initial_thread->all_elem);
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
// main 함수에서 부팅 시 호출된다.
// 선점형 커널을 위해 인터럽트를 활성화함
// 유휴 스레드를 만듦
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* Start preemptive thread scheduling. */
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. 
   idle_ticks 또는 kernel_ticks를 증가시키고 
   thread_ticks가 TIME_SLICE를 초과한 경우   */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* Update statistics. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

    if (thread_mlfqs && t != idle_thread)
        t->recent_cpu = FP_ADD_INT(t->recent_cpu, 1);

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	struct thread *curr = thread_current ();
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;
	list_push_back(&curr->child_list, &t->child_elem);
	t->parent_wait_thread = NULL;

	// t->parent_wait_thread = curr;

	/* Add to run queue. */
	thread_unblock (t);

    if (thread_current() != idle_thread) {
        list_push_back(&all_threads, &t->all_elem);
        thread_preempt();
    }

	return tid;
}

/* Returns true if A is less than B, or
   false if A is greater than or equal to B. */
bool
compare_value (const struct list_elem *elem, 
                const struct list_elem *other_elem, 
                void* offset) {
    int64_t elem_value = *((int64_t *)((uint8_t *)elem + (size_t)offset));
    int64_t other_value = *((int64_t *)((uint8_t *)other_elem + (size_t)offset));

    return (elem_value < other_value);
}

/* Returns true if A is greater than B, or
   false if A is less than or equal to B. */
bool
compare_rvalue (const struct list_elem *elem, 
                const struct list_elem *other_elem, 
                void* offset) {
    int64_t elem_value = *((int64_t *)((uint8_t *)elem + (size_t)offset));
    int64_t other_value = *((int64_t *)((uint8_t *)other_elem + (size_t)offset));

    return (elem_value > other_value);
}

/* set sleep time &  */
void
thread_sleep (int64_t ticks) {
    if (ticks <= 0) return;
	enum intr_level old_level = intr_disable ();
    struct thread *t = thread_current ();

    t->sleep_until = ticks;
    // if (thread_mlfqs)
    //     list_remove(&t->elem);
    list_insert_ordered(&sleep_list, &t->elem, compare_value, OFFSET_THREAD(sleep_until));
    thread_block();
	intr_set_level (old_level);
}

void
thread_awake (int64_t ticks) {
    struct list_elem *cur=list_begin(&sleep_list);
    struct list_elem *end=list_end(&sleep_list);
    struct thread *cur_thread;

    while (cur != end) {
        cur_thread = list_entry(cur, struct thread, elem);

        if (cur_thread->sleep_until <= ticks) {
            cur_thread->sleep_until = 0;
            cur = list_remove(cur);
            thread_unblock(cur_thread);
            thread_preempt();
        }
        else
            break;
            // cur = list_next(cur);
    }
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().
   현재 스레드를 sleep상태로 만들고 다음 스레드를 실행함.

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();

	ASSERT (t->status == THREAD_BLOCKED);
    list_push_back(&ready_list, &t->elem);
	t->status = THREAD_READY;

	intr_set_level (old_level);
}

// thread_current != idle_thread
void
thread_preempt (void) {
    if (list_empty(&ready_list)) return;
    int curr_priority = thread_current()->priority;
    int ready_priority = list_entry(list_max (&ready_list, compare_priority, OFFSET_THREAD(priority)), struct thread, elem)->priority;
    
    // if new_thread's priority is higher than current_thread's
    if (curr_priority < ready_priority) {
        if (intr_context())
            intr_yield_on_return();
        else
            thread_yield();
    }
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	struct thread *curr = thread_current ();
    list_remove (&curr->all_elem);
	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
// 현재 스레드를 다른 스레드에 양보하고 다시 스케줄한다.
// 만약 인터럽트 중이었다면 잠시 중단한다.
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, compare_rvalue, OFFSET_THREAD(priority));
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
    struct thread *t = thread_current();
    int old_priority = t->priority;

    if (thread_mlfqs)
        t->priority = new_priority;
    else {
        if (t->original_priority == t->priority) {
            t->priority = new_priority;
        }
        t->original_priority = new_priority;
    }

    if (t->priority < old_priority)
        thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) {
	/* TODO: Sets the current thread's nice value to new nice 
    and recalculates the thread's priority based on the new value (see Calculating Priority). 
    If the running thread no longer has the highest priority, yields. */
    int new_priority;

    thread_current()->nice = nice;
    new_priority = recalculate_priority(thread_current());

    thread_set_priority(new_priority);
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
    return FP_TO_INT_NEAREST(load_avg * 100);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
    struct thread *t = thread_current();
    return FP_TO_INT_NEAREST(thread_current()->recent_cpu * 100);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
// 새로운 스레드 t를 초기화 함
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
    t->sleep_until = 0;

	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);

	t->priority = priority;
	t->original_priority = priority;

	t->exit_status = -1;

    list_init(&t->lock_list);
	list_init(&t->child_list);
	t->magic = THREAD_MAGIC;
	t->is_user = false;
	t->fd_idx = 3;
	if (name == "main"){
        t->nice = 0;
        t->recent_cpu = 0;
    }
    else {
        t->nice = thread_current()->nice;
        t->recent_cpu = thread_current()->recent_cpu;
    }
}

bool
compare_priority (const struct list_elem *elem, 
                const struct list_elem *other_elem, 
                void* aux UNUSED) {
    int64_t elem_value = *((int *)((uint8_t *)elem + (size_t)OFFSET_THREAD(priority)));
    int64_t other_value = *((int *)((uint8_t *)other_elem + (size_t)OFFSET_THREAD(priority)));

    return (elem_value < other_value);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
    struct list_elem *max;
	if (list_empty (&ready_list))
		return idle_thread;
	else {
        max = list_max(&ready_list, compare_priority, OFFSET_THREAD(priority));
        list_remove(max);
		return list_entry (max, struct thread, elem);
    }
}

/* Use iretq to launch the thread */
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

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
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
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule().
 * */
/** @brief  현재 스레드의 상태를 status로 변경하고 다른 스레드로 전환
 * 
 * 현재 스레드의 상태를 인자 status로 변경.
 * 파괴할 스레드 리스트에 있는 스레드들을 모두 파괴하고
 * ready_list에 있는 다음 스레드로 문맥 전환
 * 
 * @param status 변경할 현재 스레드 상태
 */
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


/* readylist의 다음 스레드를 실행시킴 */
static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING;

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used by the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
        // 현재 스레드가 죽어가는 스레드 또는 엄한 스레드가 아니면 리스트의 맨 뒤로 보냄
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &(curr->elem));
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next);
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}


void update_priority(void) {
    struct list_elem *e;
    struct thread *t;
    int new_priority;

    for (e = list_begin (&all_threads); e != list_end (&all_threads); e = list_next (e)) {
        t = list_entry(e, struct thread, all_elem);
        t->priority = recalculate_priority(t);
    }
}

int recalculate_priority(struct thread *t) {
    int new_priority;

    new_priority = INT_TO_FP(PRI_MAX) - t->recent_cpu / 4 - INT_TO_FP(t->nice) * 2;
    new_priority = FP_TO_INT_NEAREST(new_priority);

    if (new_priority < PRI_MIN) new_priority = PRI_MIN;
    if (new_priority > PRI_MAX) new_priority = PRI_MAX;
    
    return new_priority;
}

void update_recent_cpu() {
    struct list_elem *e;
    struct thread *t;
    int k_;

    for (e = list_begin (&all_threads); e != list_end (&all_threads); e = list_next (e)) {
        t = list_entry(e, struct thread, all_elem);
        k_ = FP_DIV(2*load_avg, 2*load_avg + INT_TO_FP(1));
        t->recent_cpu = FP_MUL(k_, t->recent_cpu) + INT_TO_FP(t->nice);
    }
}

void update_load_avg(void) {
    size_t ready_threads;
    ready_threads = list_size(&ready_list);
    if (thread_current() != idle_thread)
        ready_threads++;

    load_avg = FP_MUL(INT_TO_FP(59)/60, load_avg) + FP_MUL_INT(INT_TO_FP(1)/60, ready_threads);
}