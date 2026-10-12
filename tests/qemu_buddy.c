#include "buddy_mm.h"
#include "kalloc.h"
#include "qemu_virt.h"
#include "syscall.h"
#include "mlfq.h"
#include "inode_test.h"
#include "proc_fs_test.h"

/* This smoke test manages one MiB after the kernel image. Metadata stays in
 * BSS and is excluded from the arena by __kernel_end.
 */
#define TEST_PAGES 256u

extern char __kernel_end[];
static struct buddy_allocator allocator;
static struct buddy_page metadata[TEST_PAGES];
static struct proc_table processes;
static struct mlfq scheduler;
static unsigned int execution_order[32];
static unsigned int execution_count;
/* Synthetic flat RV64 image for lifecycle checks; no user-mode entry yet. */
static const unsigned char program[] = {0x73, 0, 0, 0};
static const struct proc_image images[] = {
	{"init", program, sizeof(program), NULL, 0, 0, 0},
};

static int kalloc_check(void) {
	const ksize_t sizes[] = {1, 16, 17, 32, 64, 128, 256, 512, 1024, 2048,
				2049, 4096, 8192};
	unsigned char *objects[sizeof(sizes) / sizeof(sizes[0])];
	if (kalloc_init(&allocator) != KALLOC_OK || kalloc(0) != NULL ||
	    kalloc(KSIZE_MAX) != NULL)
		return -1;
	for (ksize_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		objects[i] = kalloc(sizes[i]);
		if (objects[i] == NULL || (uptr_t)objects[i] % _Alignof(kmax_align_t) != 0)
			return -1;
		for (ksize_t j = 0; j < sizes[i]; j++)
			objects[i][j] = (unsigned char)(i + 1);
	}
	if (kfree(objects[0] + 1) != KALLOC_NOT_ALLOCATED ||
	    kalloc_init(&allocator) != KALLOC_BUSY)
		return -1;
	for (ksize_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		for (ksize_t j = 0; j < sizes[i]; j++)
			if (objects[i][j] != (unsigned char)(i + 1))
				return -1;
		if (kfree(objects[i]) != KALLOC_OK ||
		    kfree(objects[i]) != KALLOC_NOT_ALLOCATED)
			return -1;
	}
	return allocator.free_lists[8].len == 1 ? 0 : -1;
}

static i32_t kernel_entry(void *arg) {
	unsigned int *calls = arg;
	(*calls)++;
	return 7;
}

static int callable_check(void) {
	unsigned int calls = 0;
	struct callable entry = callable_from_fni32p(kernel_entry, &calls);
	struct proc *worker;
	if (proc_create_callable(&processes, "worker", &entry, &worker) != 0 ||
	    worker->space.count != 0 || proc_activate(&processes, worker) != 0 ||
	    proc_run_callable(&processes) != 0 || calls != 1 ||
	    worker->pstatus != ZOMBIE || worker->exit_status != 7 ||
	    worker->result.tag != fi32p || worker->result.value.i32 != 7)
		return -1;
	u64_t pid = worker->pid;
	if (proc_deactivate(&processes) != 0 || proc_reap(&processes, pid) != 0 ||
	    allocator.free_lists[8].len != 1)
		return -1;
	return 0;
}

static int process_check(void) {
	struct proc *parent;
	if (proc_table_init(&processes, &allocator, images, 1) != 0 ||
	    proc_create(&processes, "init", &parent) != 0 ||
	    proc_activate(&processes, parent) != 0)
		return -1;
	parent->tf.x[PROC_A7] = SYS_FORK;
	if (syscall_dispatch(&processes) != SYSCALL_RETURN)
		return -1;
	struct proc *child = proc_find(&processes, parent->tf.x[PROC_A0]);
	if (child == NULL || child->tf.x[PROC_A0] != 0 ||
	    child->parent != parent || child->tf.pc != PROC_CODE_BASE + 4 ||
	    child->space.pages[0].backing.addr == parent->space.pages[0].backing.addr)
		return -1;
	if (proc_copyout(parent, PROC_STACK_BASE, "init", 5) != 0)
		return -1;
	parent->tf.x[PROC_A7] = SYS_EXEC;
	parent->tf.x[PROC_A0] = PROC_STACK_BASE;
	parent->tf.x[PROC_A1] = 0;
	if (syscall_dispatch(&processes) != SYSCALL_EXEC ||
	    parent->tf.pc != PROC_CODE_BASE)
		return -1;
	parent->tf.x[PROC_A7] = SYS_KILL;
	parent->tf.x[PROC_A0] = child->pid;
	if (syscall_dispatch(&processes) != SYSCALL_RETURN || !child->killed ||
	    proc_deactivate(&processes) != 0 ||
	    proc_activate(&processes, child) != PROC_ESRCH ||
	    proc_reap(&processes, child->pid) != 0 ||
	    proc_activate(&processes, parent) != 0)
		return -1;
	u64_t pid = parent->pid;
	parent->tf.x[PROC_A7] = SYS_KILL;
	parent->tf.x[PROC_A0] = pid;
	if (syscall_dispatch(&processes) != SYSCALL_EXIT ||
	    proc_reap(&processes, pid) != PROC_EBUSY ||
	    proc_deactivate(&processes) != 0 || proc_reap(&processes, pid) != 0 ||
	    allocator.free_lists[8].len != 1)
		return -1;
	return 0;
}

struct scheduled_job {
	unsigned int id;
	unsigned int phase;
	int channel;
};

static i32_t cpu_job(void *arg) {
	struct scheduled_job *job = arg;
	struct proc *p = processes.current;
	volatile u64_t stack_guard = U64_C(0x123456789abcdef0);
	uptr_t local = (uptr_t)&stack_guard;
	if (local < p->kstack.addr || local >= p->kstack.addr + BUDDY_PAGE_SIZE ||
	    mlfq_run(&scheduler) != PROC_EBUSY ||
	    mlfq_complete(&scheduler, MLFQ_YIELD, NULL) != PROC_EBUSY)
		return -1;
	for (unsigned int i = 0; i < 6; i++) {
		if (execution_count >= 32) return -2;
		execution_order[execution_count++] = job->id;
		if (mlfq_tick(&scheduler) < 0 || mlfq_checkpoint(&scheduler) != 0 ||
		    stack_guard != U64_C(0x123456789abcdef0) || processes.current != p)
			return -3;
	}
	return (i32_t)job->id;
}

static i32_t sleeping_job(void *arg) {
	struct scheduled_job *job = arg;
	volatile u64_t guard = 1234;
	job->phase = 1;
	if (mlfq_sleep(&scheduler, &job->channel) != 0 || guard != 1234)
		return -1;
	job->phase = 3;
	return 0;
}

static i32_t waking_job(void *arg) {
	struct scheduled_job *job = arg;
	if (job->phase != 1 || mlfq_wakeup(&scheduler, &job->channel) != 1)
		return -1;
	job->phase = 2;
	if (mlfq_yield(&scheduler) != 0 || job->phase != 3) return -2;
	return 0;
}

static int scheduler_check(void) {
	struct mlfq_config config = mlfq_default_config;
	config.boost_interval = 0;
	if (mlfq_init(&scheduler, &processes, &config) != 0) return -1;
	struct scheduled_job jobs[3] = {{.id = 1}, {.id = 2}, {.id = 3}};
	struct proc *workers[3];
	for (ksize_t i = 0; i < 3; i++) {
		struct callable entry = callable_from_fni32p(cpu_job, &jobs[i]);
		if (proc_create_callable(&processes, "cpu-worker", &entry, &workers[i]) != 0)
			return -1;
	}
	if (mlfq_run(&scheduler) != 0 || execution_count != 18) return -1;
	for (ksize_t i = 0; i < 6; i++)
		if (execution_order[i] != i % 3 + 1) return -1;
	for (ksize_t i = 6; i < 18; i++)
		if (execution_order[i] != ((i - 6) / 2) % 3 + 1) return -1;
	for (ksize_t i = 0; i < 3; i++) {
		if (workers[i]->pstatus != ZOMBIE || workers[i]->entry_active ||
		    workers[i]->exit_status != (int)jobs[i].id ||
		    proc_reap(&processes, workers[i]->pid) != 0)
			return -1;
	}
	struct scheduled_job sleeper = {0};
	struct callable entry = callable_from_fni32p(sleeping_job, &sleeper);
	if (proc_create_callable(&processes, "sleeper", &entry, &workers[0]) != 0)
		return -1;
	entry = callable_from_fni32p(waking_job, &sleeper);
	if (proc_create_callable(&processes, "waker", &entry, &workers[1]) != 0 ||
	    mlfq_run(&scheduler) != 0 || sleeper.phase != 3)
		return -1;
	for (ksize_t i = 0; i < 2; i++)
		if (workers[i]->exit_status != 0 || proc_reap(&processes, workers[i]->pid) != 0)
			return -1;
	/* Return idle with a suspended stack, then resume in a later run. */
	sleeper.phase = 0;
	entry = callable_from_fni32p(sleeping_job, &sleeper);
	if (proc_create_callable(&processes, "idle-sleeper", &entry, &workers[0]) != 0 ||
	    mlfq_run(&scheduler) != 0 || sleeper.phase != 1 ||
	    workers[0]->pstatus != SLEEPING || mlfq_destroy(&scheduler) != PROC_EBUSY ||
	    mlfq_wakeup(&scheduler, &sleeper.channel) != 1 ||
	    mlfq_run(&scheduler) != 0 || sleeper.phase != 3 ||
	    workers[0]->exit_status != 0 || proc_reap(&processes, workers[0]->pid) != 0)
		return -1;
	/* A killed sleeping continuation is discarded without resuming it. */
	sleeper.phase = 0;
	if (proc_create_callable(&processes, "killed-sleeper", &entry, &workers[0]) != 0 ||
	    mlfq_run(&scheduler) != 0 || proc_kill(&processes, workers[0]->pid) != 0 ||
	    mlfq_run(&scheduler) != 0 || sleeper.phase != 1 ||
	    workers[0]->entry_active || workers[0]->exit_status != -1 ||
	    proc_reap(&processes, workers[0]->pid) != 0)
		return -1;
	/* User-mode execution is unavailable; keep its queue/state intact. */
	if (proc_create(&processes, "init", &workers[0]) != 0 ||
	    mlfq_run(&scheduler) != PROC_ENOSYS || workers[0]->pstatus != RUNNABLE ||
	    !workers[0]->schedule.queued || proc_exit(&processes, workers[0], 0) != 0 ||
	    proc_reap(&processes, workers[0]->pid) != 0 ||
	    mlfq_destroy(&scheduler) != 0 || allocator.free_lists[8].len != 1)
		return -1;
	return 0;
}

static void uart_puts(const char *text) {
	volatile unsigned char *uart = (void *)(uptr_t)QEMU_UART_BASE;
	while (*text)
		*uart = (unsigned char)*text++;
}

static void qemu_exit(unsigned int code) {
	volatile unsigned int *test = (void *)(uptr_t)QEMU_TEST_BASE;
	*test = code;
	for (;;)
		__asm__ volatile("wfi");
}

void kmain(void) {
	uptr_t base = BUDDY_ALIGN_UP(__kernel_end);
	struct buddy_arena arena = {
		.base = base,
		.size = TEST_PAGES * BUDDY_PAGE_SIZE,
	};
	struct block original = {.addr = base, .order = 8};
	struct block left, right, merged, buddy;

	if (base < QEMU_RAM_BASE || base > QEMU_RAM_END ||
	    arena.size > QEMU_RAM_END - base)
		goto fail;
	if (buddy_init(&allocator, &arena, metadata, TEST_PAGES) != BUDDY_OK)
		goto fail;
	if (allocator.free_lists[8].len != 1 ||
	    buddy_page_at(&allocator, base) != metadata ||
	    metadata[0].state != BUDDY_PAGE_FREE || metadata[0].order != 8)
		goto fail;
	if (buddy_split(&arena, &original, &left, &right) != BUDDY_OK ||
	    left.addr != base || right.addr != base + BUDDY_BLOCK_BYTES(7) ||
	    left.order != 7 || right.order != 7)
		goto fail;
	if (buddy_get_buddy(&arena, &left, &buddy) != BUDDY_OK ||
	    buddy.addr != right.addr || buddy.order != right.order)
		goto fail;
	if (buddy_combine(&arena, &right, &left, &merged) != BUDDY_OK ||
	    merged.addr != original.addr || merged.order != original.order)
		goto fail;

	uart_puts("BUDDY OK\r\n");
	if (kalloc_check() != 0) {
		uart_puts("KALLOC FAIL\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("KALLOC OK\r\n");
	int inode_result = inode_test(&allocator);
	if (inode_result != 0) {
		char digits[16];
		ksize_t count = 0;
		uart_puts("INODE FAIL line ");
		do {
			digits[count++] = (char)('0' + inode_result % 10);
			inode_result /= 10;
		} while (inode_result != 0);
		while (count != 0) {
			char digit[] = {digits[--count], '\0'};
			uart_puts(digit);
		}
		uart_puts("\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("INODE OK\r\n");
	int proc_fs_result = proc_fs_test(&allocator);
	if (proc_fs_result != 0) {
		char digits[16];
		ksize_t count = 0;
		uart_puts("PROC FS FAIL line ");
		do {
			digits[count++] = (char)('0' + proc_fs_result % 10);
			proc_fs_result /= 10;
		} while (proc_fs_result != 0);
		while (count != 0) {
			char digit[] = {digits[--count], '\0'};
			uart_puts(digit);
		}
		uart_puts("\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("PROC FS OK\r\n");
	if (process_check() != 0) {
		uart_puts("PROC FAIL\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("PROC OK\r\n");
	if (callable_check() != 0) {
		uart_puts("CALLABLE PROC FAIL\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("CALLABLE PROC OK\r\n");
	if (scheduler_check() != 0) {
		uart_puts("MLFQ FAIL\r\n");
		qemu_exit(0x3333);
	}
	uart_puts("MLFQ OK\r\n");
	qemu_exit(0x5555);
fail:
	uart_puts("BUDDY FAIL\r\n");
	qemu_exit(0x3333);
}
