#ifndef PROC_H
#define PROC_H

#include "buddy_mm.h"
#include "callable.h"

#define PROC_MAX 32u
#define PROC_MAX_PAGES 32u
#define PROC_NAME_MAX 32u
#define PROC_ARG_MAX 8u
#define PROC_ARG_BYTES 128u
#define PROC_CODE_BASE UINT64_C(0x1000)
#define PROC_STACK_TOP UINT64_C(0x400000)
#define PROC_STACK_BASE (PROC_STACK_TOP - BUDDY_PAGE_SIZE)

/* These values are kernel errors, not host errno dependencies. */
enum proc_error {
        PROC_EINVAL = -1, PROC_ENOMEM = -2, PROC_ESRCH = -3,
        PROC_EBUSY = -4, PROC_ENOEXEC = -5, PROC_EFAULT = -6,
        PROC_E2BIG = -7, PROC_ENOSYS = -8
};

enum state { RUNNABLE, ZOMBIE, USED, UNUSED, RUNNING, SLEEPING };
enum proc_kind { PROC_USER, PROC_KERNEL };

/* RISC-V swtch ABI: ra, sp, then the twelve callee-saved registers. */
struct context {
        uint64_t ra, sp;
        uint64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
};

enum proc_reg { PROC_SP = 2, PROC_A0 = 10, PROC_A1 = 11, PROC_A7 = 17 };
/* Full user register snapshot; x[0] is always zero. No CSR/trap return yet. */
struct trapframe {
        uint64_t x[32];
        uint64_t pc;
};

enum proc_perm { PROC_READ = 1, PROC_WRITE = 2, PROC_EXEC = 4 };
struct proc_page {
        uint64_t va;
        struct block backing; /* One owned order-0 page, never shared. */
        unsigned int perm;
};
/* A software map, not a hardware Sv39 page table. */
struct proc_space {
        size_t count;
        struct proc_page pages[PROC_MAX_PAGES];
};

/* Built-in flat images replace filesystem paths for this stage. Code starts
 * at PROC_CODE_BASE (RX); data/BSS follow at a page boundary (RW). Entry is
 * an even byte offset within code. Image descriptors and bytes are borrowed
 * immutable kernel storage and must outlive the process table.
 */
struct proc_image {
        const char *name;
        const void *code;
        size_t code_size;
        const void *data;
        size_t data_size;
        size_t bss_size;
        uint64_t entry_offset;
};

struct mlfq;
struct proc_schedule {
        struct mlfq *owner;
        struct node ready_node;
        unsigned int level;
        uint64_t slice_left;
        uint64_t budget_left;
        const void *wait_channel;
        int queued;
        int context_ready;
        int suspended;
};

struct proc {
        uint64_t pid;
        struct proc *parent;
        enum state pstatus;
        enum proc_kind kind;
        int killed;
        int exit_status;
        char name[PROC_NAME_MAX];
        struct block kstack;
        struct context ctx;
        struct trapframe tf;
        struct proc_space space;
        /* Used only by PROC_KERNEL; entry.arg/result pointers are borrowed.
         * A zero result tag means the entry has not returned a value yet.
         */
        struct callable entry;
        struct callable_result result;
        int entry_started;
        int entry_active;
        struct proc_schedule schedule;
};

/* All calls require serialization (currently single hart, no preemption).
 * The allocator, table and image registry must stay at stable addresses.
 * Do not copy or reinitialize a live table. No file descriptors.
 */
struct proc_table {
        struct buddy_allocator *allocator;
        const struct proc_image *images;
        size_t image_count;
        uint64_t next_pid;
        struct proc *current;
        struct mlfq *scheduler;
        struct proc entries[PROC_MAX];
};

int proc_table_init(struct proc_table *table, struct buddy_allocator *allocator,
                    const struct proc_image *images, size_t image_count);
struct proc *proc_find(struct proc_table *table, uint64_t pid);
/* Create a runnable process from a registered image; output untouched on error. */
int proc_create(struct proc_table *table, const char *name, struct proc **out);
/* Create a kernel process with a validated callable and no user mappings.
 * Copies the descriptor, borrows its context, and reserves one kernel stack.
 * Invalid input or allocation failure leaves out/table resources unchanged.
 */
int proc_create_callable(struct proc_table *table, const char *name,
                         const struct callable *entry, struct proc **out);
/* Explicit synchronous runner for the current RUNNING kernel process.
 * Runs once on the caller's stack, captures any
 * supported return type, then exits. Signed 32-bit results are exit codes;
 * other return types exit with 0. Killed callbacks exit with -1.
 * The MLFQ backend switches to the process stack before invoking this API.
 * Manual callers must not deactivate/reap until this call returns. Entry
 * code may request proc_exit but must return normally. With a scheduler,
 * entries may cooperatively yield/sleep through its architecture backend.
 */
int proc_run_callable(struct proc_table *table);
/* Deep-copy pages and user registers; parent gets PID, child gets a0 == 0.
 * Never copy the parent's kernel stack or saved kernel continuation.
 * Callable kernel processes return PROC_ENOSYS (no stack continuation copy).
 */
int64_t proc_fork(struct proc_table *table, struct proc *parent);
/* Kernel API: name/argv are trusted kernel pointers; argc <= PROC_ARG_MAX.
 * On success preserve PID/parent/kstack and replace memory/registers/name.
 * On failure the original image and registers remain intact.
 * Callable kernel processes return PROC_ENOEXEC; use a new user process.
 */
int proc_exec(struct proc_table *table, struct proc *proc, const char *name,
              size_t argc, const char *const argv[]);
/* Deferred termination, including waking a sleeping target. */
int proc_kill(struct proc_table *table, uint64_t pid);
/* Exit retains memory until reap; the caller must leave the kernel stack first.
 * Reparent children to NULL. No wait syscall in this stage.
 */
int proc_exit(struct proc_table *table, struct proc *proc, int status);
int proc_reap(struct proc_table *table, uint64_t pid);
/* Manual current-process bookkeeping for trap/scheduler integration. These
 * functions neither select a process nor switch CPU context. Attached MLFQ
 * processes must be selected/released through the scheduler instead.
 */
int proc_activate(struct proc_table *table, struct proc *proc);
int proc_deactivate(struct proc_table *table);

/* Checked software translation for kernel access to user virtual memory.
 * Copyout checks the entire range before writing, including page permissions.
 */
int proc_copyin(const struct proc *proc, void *dst, uint64_t src, size_t size);
int proc_copyout(struct proc *proc, uint64_t dst, const void *src, size_t size);
int proc_copyinstr(const struct proc *proc, char *dst, uint64_t src, size_t size);

void swtch(struct context *old_ctx, struct context *new_ctx);
#endif
