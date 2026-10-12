#include "shell.h"
#include "console.h"
#include "proc_fs.h"
#include "shell_fs.h"

#define LINE_SIZE 256u
#define ARG_COUNT 12u
#define BLOCK_SLOTS 16u
#define JOB_MAX_STEPS 1024u

enum job_kind { JOB_CPU, JOB_YIELD, JOB_SLEEP };
struct shell_job {
        u64_t pid;
        enum job_kind kind;
        u64_t steps;
        int channel;
};
struct shell_block {
        u64_t id;
        struct block block;
};
static struct {
        struct buddy_allocator *allocator;
        struct proc_table *table;
        struct mlfq *scheduler;
        struct shell_job jobs[PROC_MAX];
        struct shell_block blocks[BLOCK_SLOTS];
        u64_t next_block;
} shell;

static int equal(const char *a, const char *b) {
        while (*a != '\0' && *a == *b) { a++; b++; }
        return *a == *b;
}

static int number(const char *text, u64_t *out) {
        u64_t value = 0;
        if (*text == '\0') return PROC_EINVAL;
        for (; *text != '\0'; text++) {
                if (*text < '0' || *text > '9') return PROC_EINVAL;
                unsigned int digit = (unsigned int)(*text - '0');
                if (value > (U64_MAX - digit) / 10) return PROC_EINVAL;
                value = value * 10 + digit;
        }
        *out = value;
        return 0;
}

static int signed_number(const char *text, int *out) {
        int negative = *text == '-';
        u64_t value;
        int err = number(text + negative, &value);
        if (err != 0 || value > (u64_t)I32_MAX + (unsigned int)negative)
                return PROC_EINVAL;
        *out = negative ? (int)(-(i64_t)value) : (int)value;
        return 0;
}

static const char *state_name(enum state state) {
        switch (state) {
        case RUNNABLE: return "RUNNABLE";
        case RUNNING: return "RUNNING";
        case SLEEPING: return "SLEEPING";
        case ZOMBIE: return "ZOMBIE";
        case USED: return "USED";
        case UNUSED: return "UNUSED";
        default: return "?";
        }
}

static void report(int err) {
        if (err == 0) return;
        console_puts("error ");
        console_i64(err);
        console_puts(": ");
        switch (err) {
        case PROC_EINVAL: console_puts("invalid argument"); break;
        case PROC_ENOMEM: console_puts("no free memory or process/job slots"); break;
        case PROC_ESRCH: console_puts("process not found or already exited"); break;
        case PROC_EBUSY: console_puts("resource is busy; process operations require idle/exited state"); break;
        case PROC_ENOEXEC: console_puts("image not found or not a user-image process"); break;
        case PROC_EFAULT: console_puts("invalid user-memory range"); break;
        case PROC_E2BIG: console_puts("too many or oversized arguments"); break;
        case PROC_ENOSYS: console_puts("user-image execution/kernel fork is unavailable"); break;
        case PROC_EBADF: console_puts("invalid file descriptor"); break;
        case PROC_EMFILE: console_puts("process descriptor table is full"); break;
        case PROC_ENOENT: console_puts("path does not exist"); break;
        case PROC_ENOTDIR: console_puts("path requires a directory"); break;
        case PROC_EISDIR: console_puts("operation requires a regular file"); break;
        case PROC_EEXIST: console_puts("name already exists"); break;
        case PROC_ENOTEMPTY: console_puts("directory is not empty"); break;
        case PROC_EPERM: console_puts("operation is not permitted"); break;
        case PROC_EFBIG: console_puts("file size limit exceeded"); break;
        case PROC_EOVERFLOW: console_puts("reference or identity limit reached"); break;
        case PROC_EXDEV: console_puts("different filesystems"); break;
        default: console_puts("operation failed"); break;
        }
        console_putc('\n');
}

static int get_process(const char *text, struct proc **out) {
        u64_t pid;
        if (number(text, &pid) != 0) return PROC_EINVAL;
        struct proc *p = proc_find(shell.table, pid);
        if (p == NULL) return PROC_ESRCH;
        *out = p;
        return 0;
}

static void job_event(struct shell_job *job, const char *event) {
        console_puts("job "); console_u64(job->pid);
        console_putc(' '); console_puts(event);
}

static i32_t job_entry(void *arg) {
        struct shell_job *job = arg;
        if (job->kind == JOB_SLEEP) {
                job_event(job, "sleeping\n");
                int err = mlfq_sleep(shell.scheduler, &job->channel);
                if (err != 0) return err;
                job_event(job, "woke\n");
        }
        for (u64_t i = 0; i < job->steps; i++) {
                struct proc *p = shell.table->current;
                job_event(job, "step="); console_u64(i + 1);
                console_puts(" level="); console_u64(p->schedule.level);
                console_puts(" budget="); console_u64(p->schedule.budget_left);
                console_putc('\n');
                int err = mlfq_tick(shell.scheduler);
                if (err < 0) return err;
                err = job->kind == JOB_YIELD ? mlfq_yield(shell.scheduler) :
                                               mlfq_checkpoint(shell.scheduler);
                if (err != 0) return err;
        }
        job_event(job, "done\n");
        return 0;
}

static int spawn(enum job_kind kind, u64_t steps) {
        struct shell_job *job = NULL;
        for (ksize_t i = 0; i < PROC_MAX; i++)
                if (shell.jobs[i].pid == 0 || proc_find(shell.table, shell.jobs[i].pid) == NULL) {
                        job = &shell.jobs[i];
                        break;
                }
        if (job == NULL) return PROC_ENOMEM;
        const char *name = kind == JOB_CPU ? "cpu" : kind == JOB_YIELD ? "yield" : "sleep";
        struct callable entry = callable_from_fni32p(job_entry, job);
        struct proc *p;
        int err = proc_create_callable(shell.table, name, &entry, &p);
        if (err != 0) return err;
        *job = (struct shell_job){ .pid = p->pid, .kind = kind, .steps = steps };
        console_puts("spawned pid="); console_u64(p->pid);
        console_puts(" name="); console_puts(name);
        console_puts(" steps="); console_u64(steps); console_putc('\n');
        return 0;
}

static int cmd_help(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts(
                "help                         show commands\n"
                "echo [words...]              print words\n"
                "pwd / cd [PATH]              show/change shell cwd; cd defaults to /\n"
                "mkdir PATH / touch PATH      create directory / empty file\n"
                "ls [PATH] / cat PATH         list entries / show file contents\n"
                "rm PATH                      remove a file or empty directory\n"
                "editor PATH (or edit PATH)   line editor; w saves, q! discards\n"
                "mem                          inspect buddy memory\n"
                "alloc ORDER / free ID|all    allocate/release a tracked buddy block\n"
                "ps / show PID                inspect processes and saved registers\n"
                "queues                       inspect MLFQ queues and budgets\n"
                "spawn cpu|yield|sleep [N]     create callable job (1..1024 steps)\n"
                "run                          execute queued kernel jobs until idle\n"
                "wake PID|all                 wake sleeping jobs\n"
                "kill PID|all                 request termination; run to finish it\n"
                "reap PID|all                 reclaim zombie resources\n"
                "exit PID [CODE]              mark a non-running process exited\n"
                "boost / tick [N]             boost priorities / advance test clock\n"
                "image PATH                   load a saved user image (init or alt)\n"
                "fork PID                     deep-copy a user-image process\n"
                "exec PID PATH [args...]       load an executable inode\n"
                "mkdir/chdir/unlink PID PATH  process cwd operations (test commands)\n"
                "open PID PATH MODE           MODE: r, w, rw, create, truncate\n"
                "close/dup PID FD             close or share an open cursor\n"
                "read PID FD N                read up to 128 bytes\n"
                "write PID FD TEXT            write one whitespace-free token\n"
                "demo                         queue 3 CPU jobs and a sleeping job, then run\n"
                "quit                         power off QEMU\n"
                "Editing: backspace, Ctrl-U clear, Ctrl-C cancel, Ctrl-D quit.\n"
                "Paths may use single/double quotes or backslash escapes.\n"
                "User images support lifecycle inspection; run executes callable jobs.\n");
        return 0;
}

static int cmd_echo(ksize_t argc, char **argv) {
        for (ksize_t i = 1; i < argc; i++) {
                if (i != 1) console_putc(' ');
                console_puts(argv[i]);
        }
        console_putc('\n');
        return 0;
}

static int cmd_mem(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        ksize_t free_count = 0;
        for (int order = 0; order <= BUDDY_MAX_ORDER; order++)
                free_count += shell.allocator->free_lists[order].len * ((ksize_t)1 << order);
        console_puts("pages total="); console_u64(shell.allocator->page_count);
        console_puts(" free="); console_u64(free_count);
        console_puts(" used="); console_u64(shell.allocator->page_count - free_count);
        console_puts(" page_size="); console_u64(BUDDY_PAGE_SIZE); console_putc('\n');
        for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
                ksize_t blocks = shell.allocator->free_lists[order].len;
                if (blocks == 0) continue;
                console_puts("  order="); console_u64((u64_t)order);
                console_puts(" blocks="); console_u64(blocks); console_putc('\n');
        }
        return 0;
}

static int cmd_alloc(ksize_t argc, char **argv) {
        (void)argc;
        u64_t order;
        if (number(argv[1], &order) != 0 || order > BUDDY_MAX_ORDER) return PROC_EINVAL;
        for (ksize_t i = 0; i < BLOCK_SLOTS; i++) {
                struct shell_block *slot = &shell.blocks[i];
                if (slot->id != 0) continue;
                if (shell.next_block == 0) return PROC_E2BIG;
                if (buddy_alloc(shell.allocator, (int)order, &slot->block) != BUDDY_OK)
                        return PROC_ENOMEM;
                slot->id = shell.next_block++;
                console_puts("block id="); console_u64(slot->id);
                console_puts(" address="); console_hex(slot->block.addr);
                console_puts(" bytes="); console_u64(BUDDY_BLOCK_BYTES(order));
                console_putc('\n');
                return 0;
        }
        return PROC_E2BIG;
}

static int cmd_free(ksize_t argc, char **argv) {
        (void)argc;
        int all = equal(argv[1], "all");
        u64_t id = 0;
        if (!all && number(argv[1], &id) != 0) return PROC_EINVAL;
        ksize_t count = 0;
        for (ksize_t i = 0; i < BLOCK_SLOTS; i++) {
                struct shell_block *slot = &shell.blocks[i];
                if (slot->id == 0 || (!all && slot->id != id)) continue;
                if (buddy_free(shell.allocator, &slot->block) != BUDDY_OK) return PROC_EINVAL;
                slot->id = 0;
                count++;
        }
        if (!all && count == 0) return PROC_EINVAL;
        console_puts("freed blocks="); console_u64(count); console_putc('\n');
        return 0;
}

static int cmd_ps(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts("PID PARENT KIND STATE LEVEL SLICE BUDGET KILLED EXIT NAME\n");
        ksize_t count = 0;
        for (ksize_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &shell.table->entries[i];
                if (p->pstatus == UNUSED) continue;
                count++;
                console_u64(p->pid); console_putc(' ');
                console_u64(p->parent == NULL ? 0 : p->parent->pid); console_putc(' ');
                console_puts(p->kind == PROC_KERNEL ? "kernel " : "user ");
                console_puts(state_name(p->pstatus)); console_putc(' ');
                console_u64(p->schedule.level); console_putc(' ');
                console_u64(p->schedule.slice_left); console_putc(' ');
                console_u64(p->schedule.budget_left); console_putc(' ');
                console_i64(p->killed); console_putc(' ');
                console_i64(p->exit_status); console_putc(' ');
                console_puts(p->name); console_putc('\n');
        }
        console_puts("processes="); console_u64(count); console_putc('\n');
        return 0;
}

static int cmd_show(ksize_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        console_puts("pid="); console_u64(p->pid);
        console_puts(" state="); console_puts(state_name(p->pstatus));
        console_puts(" exit="); console_i64(p->exit_status); console_putc('\n');
        console_puts("pc="); console_hex(p->tf.pc);
        console_puts(" user_sp="); console_hex(p->tf.x[PROC_SP]);
        console_puts(" a0="); console_u64(p->tf.x[PROC_A0]);
        console_puts(" a1="); console_hex(p->tf.x[PROC_A1]); console_putc('\n');
        console_puts("kernel_stack="); console_hex(p->kstack.addr);
        console_puts(" started="); console_i64(p->entry_started);
        console_puts(" active="); console_i64(p->entry_active);
        console_puts(" result_tag="); console_u64(p->result.tag);
        if (p->result.tag == fi32v || p->result.tag == fi32p) {
                console_puts(" result="); console_i64(p->result.value.i32);
        }
        console_putc('\n');
        console_puts("cwd_inum="); console_u64(p->cwd == NULL ? 0 : p->cwd->inum);
        console_puts(" fds:");
        for (ksize_t fd = 0; fd < PROC_FD_MAX; fd++) {
                if (p->files[fd] == NULL) continue;
                console_putc(' '); console_u64(fd);
        }
        console_putc('\n');
        console_puts("mapped_pages="); console_u64(p->space.count); console_putc('\n');
        for (ksize_t i = 0; i < p->space.count; i++) {
                struct proc_page *page = &p->space.pages[i];
                console_puts("  va="); console_hex(page->va);
                console_puts(" physical="); console_hex(page->backing.addr);
                console_puts(" perms=");
                console_putc(page->perm & PROC_READ ? 'r' : '-');
                console_putc(page->perm & PROC_WRITE ? 'w' : '-');
                console_putc(page->perm & PROC_EXEC ? 'x' : '-');
                console_putc('\n');
        }
        return 0;
}

static int cmd_queues(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts("ticks="); console_u64(shell.scheduler->ticks);
        console_puts(" boost_interval="); console_u64(shell.scheduler->config.boost_interval);
        console_putc('\n');
        for (ksize_t i = 0; i < MLFQ_LEVELS; i++) {
                console_puts("level="); console_u64(i);
                console_puts(" quantum="); console_u64(shell.scheduler->config.quantum[i]);
                console_puts(" allotment="); console_u64(shell.scheduler->config.allotment[i]);
                console_puts(" ready="); console_u64(shell.scheduler->queues[i].len);
                console_puts(" pids:");
                for (struct node *node = shell.scheduler->queues[i].head; node; node = node->next) {
                        struct proc *p = node->data;
                        console_putc(' '); console_u64(p->pid);
                }
                console_putc('\n');
        }
        return 0;
}

static int cmd_spawn(ksize_t argc, char **argv) {
        enum job_kind kind;
        if (equal(argv[1], "cpu")) kind = JOB_CPU;
        else if (equal(argv[1], "yield")) kind = JOB_YIELD;
        else if (equal(argv[1], "sleep")) kind = JOB_SLEEP;
        else return PROC_EINVAL;
        u64_t steps = 6;
        if (argc == 3 && number(argv[2], &steps) != 0) return PROC_EINVAL;
        if (steps == 0 || steps > JOB_MAX_STEPS) return PROC_EINVAL;
        return spawn(kind, steps);
}

static int cmd_run(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        int err = mlfq_run(shell.scheduler);
        if (err != 0) return err;
        console_puts("scheduler idle\n");
        return 0;
}

static int cmd_wake(ksize_t argc, char **argv) {
        (void)argc;
        int count = 0;
        if (equal(argv[1], "all")) {
                for (ksize_t i = 0; i < PROC_MAX; i++) {
                        struct proc *p = &shell.table->entries[i];
                        if (p->pstatus == SLEEPING && p->schedule.wait_channel != NULL) {
                                int n = mlfq_wakeup(shell.scheduler, p->schedule.wait_channel);
                                if (n < 0) return n;
                                count += n;
                        }
                }
        } else {
                struct proc *p;
                int err = get_process(argv[1], &p);
                if (err != 0) return err;
                if (p->pstatus != SLEEPING || p->schedule.wait_channel == NULL)
                        return PROC_EINVAL;
                count = mlfq_wakeup(shell.scheduler, p->schedule.wait_channel);
        }
        console_puts("woken="); console_i64(count); console_putc('\n');
        return 0;
}

static int cmd_kill(ksize_t argc, char **argv) {
        (void)argc;
        ksize_t count = 0;
        if (equal(argv[1], "all")) {
                for (ksize_t i = 0; i < PROC_MAX; i++) {
                        struct proc *p = &shell.table->entries[i];
                        if (p->pstatus == UNUSED || p->pstatus == ZOMBIE) continue;
                        int err = proc_kill(shell.table, p->pid);
                        if (err != 0) return err;
                        count++;
                }
        } else {
                struct proc *p;
                int err = get_process(argv[1], &p);
                if (err != 0) return err;
                err = proc_kill(shell.table, p->pid);
                if (err != 0) return err;
                count = 1;
        }
        console_puts("kill requested="); console_u64(count); console_putc('\n');
        return 0;
}

static int cmd_reap(ksize_t argc, char **argv) {
        (void)argc;
        ksize_t count = 0;
        if (equal(argv[1], "all")) {
                for (ksize_t i = 0; i < PROC_MAX; i++) {
                        struct proc *p = &shell.table->entries[i];
                        if (p->pstatus != ZOMBIE) continue;
                        int err = proc_reap(shell.table, p->pid);
                        if (err != 0) return err;
                        count++;
                }
        } else {
                struct proc *p;
                int err = get_process(argv[1], &p);
                if (err != 0) return err;
                err = proc_reap(shell.table, p->pid);
                if (err != 0) return err;
                count = 1;
        }
        console_puts("reaped="); console_u64(count); console_putc('\n');
        return 0;
}

static int cmd_exit(ksize_t argc, char **argv) {
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        int status = 0;
        if (argc == 3 && signed_number(argv[2], &status) != 0) return PROC_EINVAL;
        err = proc_exit(shell.table, p, status);
        if (err != 0) return err;
        console_puts("exited pid="); console_u64(p->pid);
        console_puts(" code="); console_i64(status); console_putc('\n');
        return 0;
}

static int cmd_boost(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        int err = mlfq_boost(shell.scheduler);
        if (err == 0) console_puts("priorities boosted\n");
        return err;
}

static int cmd_tick(ksize_t argc, char **argv) {
        u64_t count = 1;
        if (argc == 2 && number(argv[1], &count) != 0) return PROC_EINVAL;
        if (count == 0 || count > 65536) return PROC_EINVAL;
        for (u64_t i = 0; i < count; i++) {
                int err = mlfq_tick(shell.scheduler);
                if (err < 0) return err;
        }
        console_puts("ticks="); console_u64(shell.scheduler->ticks); console_putc('\n');
        return 0;
}

static int cmd_image(ksize_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = proc_create(shell.table, argv[1], &p);
        if (err != 0) return err;
        console_puts("image pid="); console_u64(p->pid);
        console_puts(" name="); console_puts(p->name); console_putc('\n');
        return 0;
}

static int cmd_fork(ksize_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        i64_t pid = proc_fork(shell.table, p);
        if (pid < 0) return (int)pid;
        console_puts("fork child="); console_i64(pid);
        console_puts(" parent="); console_u64(p->pid); console_putc('\n');
        return 0;
}

static int cmd_exec(ksize_t argc, char **argv) {
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        if (argc - 3 > PROC_ARG_MAX) return PROC_E2BIG;
        const char *args[PROC_ARG_MAX];
        for (ksize_t i = 3; i < argc; i++) args[i - 3] = argv[i];
        err = proc_exec(shell.table, p, argv[2], argc - 3, args);
        if (err != 0) return err;
        console_puts("exec pid="); console_u64(p->pid);
        console_puts(" name="); console_puts(p->name);
        console_puts(" argc="); console_u64(p->tf.x[PROC_A0]);
        console_puts(" entry="); console_hex(p->tf.pc); console_putc('\n');
        return 0;
}

static int cmd_demo(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        for (ksize_t i = 0; i < PROC_MAX; i++)
                if (shell.table->entries[i].pstatus != UNUSED) return PROC_EBUSY;
        for (ksize_t i = 0; i < 3; i++) {
                int err = spawn(JOB_CPU, 6);
                if (err != 0) return err;
        }
        int err = spawn(JOB_SLEEP, 2);
        if (err != 0) return err;
        err = cmd_run(0, NULL);
        if (err == 0)
                console_puts("Demo: ps; wake all; run; reap all; mem\n");
        return err;
}

static int cmd_proc_path(ksize_t argc, char **argv) {
        if (argc == 2) return shell_fs_command(argc, argv);
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        if (equal(argv[0], "mkdir")) err = proc_mkdir(shell.table, p, argv[2]);
        else if (equal(argv[0], "chdir")) err = proc_chdir(shell.table, p, argv[2]);
        else err = proc_unlink(shell.table, p, argv[2]);
        if (err == 0) console_puts("filesystem operation OK\n");
        return err;
}

static int cmd_open(ksize_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        unsigned int flags;
        if (equal(argv[3], "r")) flags = PROC_O_READ;
        else if (equal(argv[3], "w")) flags = PROC_O_WRITE;
        else if (equal(argv[3], "rw")) flags = PROC_O_READ | PROC_O_WRITE;
        else if (equal(argv[3], "create")) flags = PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE;
        else if (equal(argv[3], "truncate")) flags = PROC_O_READ | PROC_O_WRITE | PROC_O_TRUNC;
        else return PROC_EINVAL;
        int fd = proc_open(shell.table, p, argv[2], flags);
        if (fd < 0) return fd;
        console_puts("opened fd="); console_i64(fd); console_putc('\n');
        return 0;
}

static int cmd_descriptor(ksize_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        u64_t fd;
        if (number(argv[2], &fd) != 0 || fd >= PROC_FD_MAX) return PROC_EBADF;
        if (equal(argv[0], "close")) {
                err = proc_close(shell.table, p, (int)fd);
                if (err == 0) console_puts("closed descriptor\n");
                return err;
        }
        if (equal(argv[0], "dup")) {
                int result = proc_dup(shell.table, p, (int)fd);
                if (result < 0) return result;
                console_puts("duplicated fd="); console_i64(result); console_putc('\n');
                return 0;
        }
        i64_t result;
        if (equal(argv[0], "write")) {
                ksize_t count = 0;
                while (argv[3][count] != '\0') count++;
                result = proc_write(shell.table, p, (int)fd, argv[3], count);
                if (result < 0) return (int)result;
                console_puts("wrote bytes="); console_i64(result); console_putc('\n');
        } else {
                u64_t count;
                char buffer[128];
                if (number(argv[3], &count) != 0 || count > sizeof(buffer)) return PROC_EINVAL;
                result = proc_read(shell.table, p, (int)fd, buffer, count);
                if (result < 0) return (int)result;
                console_puts("read bytes="); console_i64(result); console_puts(" data=");
                for (i64_t i = 0; i < result; i++) console_putc(buffer[i]);
                console_putc('\n');
        }
        return 0;
}

static int cmd_quit(ksize_t argc, char **argv) {
        (void)argc; (void)argv;
        shell_fs_destroy();
        console_puts("bye\n");
        console_shutdown(0);
}

struct command {
        const char *name;
        ksize_t min_args, max_args;
        int (*run)(ksize_t argc, char **argv);
};
static const struct command commands[] = {
        {"help", 1, 1, cmd_help}, {"echo", 1, ARG_COUNT, cmd_echo},
        {"pwd", 1, 1, shell_fs_command}, {"cd", 1, 2, shell_fs_command},
        {"touch", 2, 2, shell_fs_command}, {"ls", 1, 2, shell_fs_command},
        {"cat", 2, 2, shell_fs_command}, {"rm", 2, 2, shell_fs_command},
        {"editor", 2, 2, shell_fs_command}, {"edit", 2, 2, shell_fs_command},
        {"mem", 1, 1, cmd_mem}, {"alloc", 2, 2, cmd_alloc}, {"free", 2, 2, cmd_free},
        {"ps", 1, 1, cmd_ps}, {"show", 2, 2, cmd_show}, {"queues", 1, 1, cmd_queues},
        {"spawn", 2, 3, cmd_spawn}, {"run", 1, 1, cmd_run}, {"wake", 2, 2, cmd_wake},
        {"kill", 2, 2, cmd_kill}, {"reap", 2, 2, cmd_reap}, {"exit", 2, 3, cmd_exit},
        {"boost", 1, 1, cmd_boost}, {"tick", 1, 2, cmd_tick},
        {"image", 2, 2, cmd_image}, {"fork", 2, 2, cmd_fork}, {"exec", 3, 11, cmd_exec},
        {"mkdir", 2, 3, cmd_proc_path}, {"chdir", 3, 3, cmd_proc_path},
        {"unlink", 3, 3, cmd_proc_path}, {"open", 4, 4, cmd_open},
        {"close", 3, 3, cmd_descriptor}, {"dup", 3, 3, cmd_descriptor},
        {"read", 4, 4, cmd_descriptor}, {"write", 4, 4, cmd_descriptor},
        {"demo", 1, 1, cmd_demo}, {"quit", 1, 1, cmd_quit},
};

static int execute(char *line) {
        char *args[ARG_COUNT];
        ksize_t count = 0;
        char *read = line, *write = line;
        while (*read != '\0') {
                while (*read == ' ' || *read == '\t') read++;
                if (*read == '\0') break;
                if (count == ARG_COUNT) return PROC_E2BIG;
                args[count++] = write;
                char quote = '\0';
                while (*read != '\0') {
                        char ch = *read++;
                        if (quote == '\0' && (ch == ' ' || ch == '\t')) break;
                        if ((ch == '\'' || ch == '"') && (quote == '\0' || quote == ch)) {
                                quote = quote == '\0' ? ch : '\0';
                        } else if (ch == '\\' && quote != '\'') {
                                if (*read == '\0') {
                                        console_puts("unfinished escape\n"); return PROC_EINVAL;
                                }
                                *write++ = *read++;
                        } else *write++ = ch;
                }
                if (quote != '\0') {
                        console_puts("unterminated quote\n"); return PROC_EINVAL;
                }
                *write++ = '\0';
        }
        if (count == 0) return 0;
        for (ksize_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
                const struct command *command = &commands[i];
                if (!equal(args[0], command->name)) continue;
                if (count < command->min_args || count > command->max_args) {
                        console_puts("wrong argument count; use help\n");
                        return PROC_EINVAL;
                }
                return command->run(count, args);
        }
        console_puts("unknown command; use help\n");
        return PROC_EINVAL;
}

_Noreturn void shell_main_loop(struct buddy_allocator *allocator,
                               struct proc_table *table, struct mlfq *scheduler) {
        shell.allocator = allocator;
        shell.table = table;
        shell.scheduler = scheduler;
        shell.next_block = 1;
        int err = shell_fs_init(table->filesystem);
        if (err != 0) { report(err); console_shutdown(1); }
        console_puts("\nos_learn stage shell\n"
                     "RAM filesystem + processes + cooperative MLFQ\n"
                     "Type help for commands, ls to browse, or editor FILE to edit text.\n");
        for (;;) {
                char line[LINE_SIZE];
                console_puts("os> ");
                int status = console_readline(line, sizeof(line));
                if (status == 1) (void)cmd_quit(0, NULL);
                if (status < 0) {
                        console_puts("error: line too long (maximum 255 characters)\n");
                        continue;
                }
                report(execute(line));
        }
}
