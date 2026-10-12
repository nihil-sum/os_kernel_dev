#ifndef PROC_H
#define PROC_H

#include "buddy_mm.h"
#include "callable.h"
#include "fs.h"

#define PROC_MAX 32u
#define PROC_MAX_PAGES 32u
#define PROC_NAME_MAX 32u
#define PROC_ARG_MAX 8u
#define PROC_ARG_BYTES 128u
#define PROC_FD_MAX 16u
#define PROC_CODE_BASE U64_C(0x1000)
#define PROC_STACK_TOP U64_C(0x400000)
#define PROC_STACK_BASE (PROC_STACK_TOP - BUDDY_PAGE_SIZE)

/* These values are kernel errors, not host errno dependencies. */
enum proc_error {
        PROC_EINVAL = -1, PROC_ENOMEM = -2, PROC_ESRCH = -3,
        PROC_EBUSY = -4, PROC_ENOEXEC = -5, PROC_EFAULT = -6,
        PROC_E2BIG = -7, PROC_ENOSYS = -8,
        PROC_EBADF = -9, PROC_EMFILE = -10, PROC_ENOENT = -11,
        PROC_ENOTDIR = -12, PROC_EISDIR = -13, PROC_EEXIST = -14,
        PROC_ENOTEMPTY = -15, PROC_EPERM = -16, PROC_EFBIG = -17,
        PROC_EOVERFLOW = -18, PROC_EXDEV = -19
};

enum state { RUNNABLE, ZOMBIE, USED, UNUSED, RUNNING, SLEEPING };
enum proc_kind { PROC_USER, PROC_KERNEL };

/* RISC-V swtch ABI: ra, sp, then the twelve callee-saved registers. */
struct context {
        u64_t ra, sp;
        u64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
};

enum proc_reg { PROC_SP = 2, PROC_A0 = 10, PROC_A1 = 11, PROC_A2 = 12, PROC_A7 = 17 };
/* Full user register snapshot; x[0] is always zero. No CSR/trap return yet. */
struct trapframe {
        u64_t x[32];
        u64_t pc;
};

enum proc_perm { PROC_READ = 1, PROC_WRITE = 2, PROC_EXEC = 4 };
struct proc_page {
        u64_t va;
        struct block backing; /* One owned order-0 page, never shared. */
        unsigned int perm;
};
/* A software map, not a hardware Sv39 page table. */
struct proc_space {
        ksize_t count;
        struct proc_page pages[PROC_MAX_PAGES];
};

/* Bootstrap image descriptor. Code starts
 * at PROC_CODE_BASE (RX); data/BSS follow at a page boundary (RW). Entry is
 * an even byte offset within code. Image descriptors and bytes are borrowed
 * immutable kernel storage. proc_image_install copies it into an inode.
 * The legacy proc_table_init registry borrows descriptors for host tests.
 */
struct proc_image {
        const char *name;
        const void *code;
        ksize_t code_size;
        const void *data;
        ksize_t data_size;
        ksize_t bss_size;
        u64_t entry_offset;
};

/* Small native RV64 little-endian executable format, not ELF: this header,
 * then code_size code bytes, then data_size data bytes, with no padding.
 * The magic includes format version 1. BSS is zeroed by the loader.
 */
#define PROC_EXEC_MAGIC U64_C(0x31434558454c534f)
struct proc_exec_header {
        u64_t magic, code_size, data_size, bss_size, entry_offset;
};
_Static_assert(sizeof(struct proc_exec_header) == 40, "executable header layout");

struct proc_file;

struct mlfq;
struct proc_schedule {
        struct mlfq *owner;
        struct node ready_node;
        unsigned int level;
        u64_t slice_left;
        u64_t budget_left;
        const void *wait_channel;
        int queued;
        int context_ready;
        int suspended;
};

struct proc {
        u64_t pid;
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
        struct inode *cwd; /* Owned reference; inherited by fork. */
        struct proc_file *files[PROC_FD_MAX]; /* Shared open descriptions. */
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
 * Allocator, table, and filesystem must stay at stable addresses.
 * Do not copy or reinitialize a live table. fs_root pins the mount until
 * proc_table_destroy; filesystem is borrowed while that reference is held.
 */
struct proc_table {
        struct buddy_allocator *allocator;
        struct fs *filesystem;
        struct inode *fs_root;
        const struct proc_image *images;
        ksize_t image_count;
        u64_t next_pid;
        struct proc *current;
        struct mlfq *scheduler;
        struct proc entries[PROC_MAX];
};

int proc_table_init(struct proc_table *table, struct buddy_allocator *allocator,
                    const struct proc_image *images, ksize_t image_count);
/* Preferred initializer: kalloc and filesystem must already be initialized.
 * New processes start at root with no descriptors; fork inherits both.
 * Failure leaves table unchanged. Call on fresh storage, never a live table.
 */
int proc_table_init_fs(struct proc_table *table, struct buddy_allocator *allocator,
                       struct fs *filesystem);
/* Requires every process reaped and the scheduler detached. */
int proc_table_destroy(struct proc_table *table);
/* Bootstrap a named executable, publishing only after all writes succeed. */
int proc_image_install(struct inode *directory, const char *name,
                        const struct proc_image *image);
struct proc *proc_find(struct proc_table *table, u64_t pid);
/* Create a runnable process from a filesystem path (relative to root).
 * The legacy initializer resolves registry names. Output untouched on error.
 */
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
/* Deep-copy pages/registers; retain cwd and share open descriptions/offsets.
 * Parent gets PID, child gets a0 == 0. Failure unwinds all child ownership.
 * Never copy the parent's kernel stack or saved kernel continuation.
 * Callable kernel processes return PROC_ENOSYS (no stack continuation copy).
 */
i64_t proc_fork(struct proc_table *table, struct proc *parent);
/* Kernel API: name/argv are trusted kernel pointers; argc <= PROC_ARG_MAX.
 * Filesystem tables resolve a path relative to cwd (or root for '/').
 * On success preserve PID/parent/kstack/cwd/descriptors and replace image.
 * On failure the original image and registers remain intact.
 * Callable kernel processes return PROC_ENOEXEC; use a new user process.
 */
int proc_exec(struct proc_table *table, struct proc *proc, const char *name,
              ksize_t argc, const char *const argv[]);
/* Deferred termination, including waking a sleeping target. */
int proc_kill(struct proc_table *table, u64_t pid);
/* Exit closes descriptors and releases cwd immediately, retaining memory until
 * reap; the caller must leave the kernel stack before reap.
 * Reparent children to NULL. No wait syscall in this stage.
 */
int proc_exit(struct proc_table *table, struct proc *proc, int status);
int proc_reap(struct proc_table *table, u64_t pid);
/* Manual current-process bookkeeping for trap/scheduler integration. These
 * functions neither select a process nor switch CPU context. Attached MLFQ
 * processes must be selected/released through the scheduler instead.
 */
int proc_activate(struct proc_table *table, struct proc *proc);
int proc_deactivate(struct proc_table *table);

/* Checked software translation for kernel access to user virtual memory.
 * Copyout checks the entire range before writing, including page permissions.
 */
int proc_copyin(const struct proc *proc, void *dst, u64_t src, ksize_t size);
int proc_copyout(struct proc *proc, u64_t dst, const void *src, ksize_t size);
int proc_copyinstr(const struct proc *proc, char *dst, u64_t src, ksize_t size);
/* Validate an entire user range before syscall I/O changes an open offset. */
int proc_user_range(const struct proc *proc, u64_t va, ksize_t size,
                     unsigned int perm);

void swtch(struct context *old_ctx, struct context *new_ctx);
#endif
