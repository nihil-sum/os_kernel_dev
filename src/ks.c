#include "shell.h"
#include "console.h"

#define LINE_SIZE 256u
#define ARG_COUNT 12u
#define BLOCK_SLOTS 16u
#define JOB_MAX_STEPS 1024u

enum job_kind { JOB_CPU, JOB_YIELD, JOB_SLEEP };
struct shell_job {
        uint64_t pid;
        enum job_kind kind;
        uint64_t steps;
        int channel;
};
struct shell_block {
        uint64_t id;
        struct block block;
};
static struct {
        struct buddy_allocator *allocator;
        struct proc_table *table;
        struct mlfq *scheduler;
        struct shell_job jobs[PROC_MAX];
        struct shell_block blocks[BLOCK_SLOTS];
        uint64_t next_block;
} shell;

static int equal(const char *a, const char *b) {
        while (*a != '\0' && *a == *b) { a++; b++; }
        return *a == *b;
}

static int number(const char *text, uint64_t *out) {
        uint64_t value = 0;
        if (*text == '\0') return PROC_EINVAL;
        for (; *text != '\0'; text++) {
                if (*text < '0' || *text > '9') return PROC_EINVAL;
                unsigned int digit = (unsigned int)(*text - '0');
                if (value > (UINT64_MAX - digit) / 10) return PROC_EINVAL;
                value = value * 10 + digit;
        }
        *out = value;
        return 0;
}

static int signed_number(const char *text, int *out) {
        int negative = *text == '-';
        uint64_t value;
        int err = number(text + negative, &value);
        if (err != 0 || value > (uint64_t)INT32_MAX + (unsigned int)negative)
                return PROC_EINVAL;
        *out = negative ? (int)(-(int64_t)value) : (int)value;
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
        case PROC_EBUSY: console_puts("operation requires an idle or exited process"); break;
        case PROC_ENOEXEC: console_puts("image not found or not a user-image process"); break;
        case PROC_EFAULT: console_puts("invalid user-memory range"); break;
        case PROC_E2BIG: console_puts("too many or oversized arguments"); break;
        case PROC_ENOSYS: console_puts("user-image execution/kernel fork is unavailable"); break;
        default: console_puts("operation failed"); break;
        }
        console_putc('\n');
}

static int get_process(const char *text, struct proc **out) {
        uint64_t pid;
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

static int32_t job_entry(void *arg) {
        struct shell_job *job = arg;
        if (job->kind == JOB_SLEEP) {
                job_event(job, "sleeping\n");
                int err = mlfq_sleep(shell.scheduler, &job->channel);
                if (err != 0) return err;
                job_event(job, "woke\n");
        }
        for (uint64_t i = 0; i < job->steps; i++) {
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

static int spawn(enum job_kind kind, uint64_t steps) {
        struct shell_job *job = NULL;
        for (size_t i = 0; i < PROC_MAX; i++)
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

static int cmd_help(size_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts(
                "help                         show commands\n"
                "echo [words...]              print words\n"
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
                "image init|alt               create a saved user image\n"
                "fork PID                     deep-copy a user-image process\n"
                "exec PID init|alt [args...]   replace a saved user image\n"
                "demo                         queue 3 CPU jobs and a sleeping job, then run\n"
                "quit                         power off QEMU\n"
                "Editing: backspace, Ctrl-U clear, Ctrl-C cancel, Ctrl-D quit.\n"
                "User images support lifecycle inspection; run executes callable jobs.\n");
        return 0;
}

static int cmd_echo(size_t argc, char **argv) {
        for (size_t i = 1; i < argc; i++) {
                if (i != 1) console_putc(' ');
                console_puts(argv[i]);
        }
        console_putc('\n');
        return 0;
}

static int cmd_mem(size_t argc, char **argv) {
        (void)argc; (void)argv;
        size_t free_count = 0;
        for (int order = 0; order <= BUDDY_MAX_ORDER; order++)
                free_count += shell.allocator->free_lists[order].len * ((size_t)1 << order);
        console_puts("pages total="); console_u64(shell.allocator->page_count);
        console_puts(" free="); console_u64(free_count);
        console_puts(" used="); console_u64(shell.allocator->page_count - free_count);
        console_puts(" page_size="); console_u64(BUDDY_PAGE_SIZE); console_putc('\n');
        for (int order = 0; order <= BUDDY_MAX_ORDER; order++) {
                size_t blocks = shell.allocator->free_lists[order].len;
                if (blocks == 0) continue;
                console_puts("  order="); console_u64((uint64_t)order);
                console_puts(" blocks="); console_u64(blocks); console_putc('\n');
        }
        return 0;
}

static int cmd_alloc(size_t argc, char **argv) {
        (void)argc;
        uint64_t order;
        if (number(argv[1], &order) != 0 || order > BUDDY_MAX_ORDER) return PROC_EINVAL;
        for (size_t i = 0; i < BLOCK_SLOTS; i++) {
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

static int cmd_free(size_t argc, char **argv) {
        (void)argc;
        int all = equal(argv[1], "all");
        uint64_t id = 0;
        if (!all && number(argv[1], &id) != 0) return PROC_EINVAL;
        size_t count = 0;
        for (size_t i = 0; i < BLOCK_SLOTS; i++) {
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

static int cmd_ps(size_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts("PID PARENT KIND STATE LEVEL SLICE BUDGET KILLED EXIT NAME\n");
        size_t count = 0;
        for (size_t i = 0; i < PROC_MAX; i++) {
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

static int cmd_show(size_t argc, char **argv) {
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
        console_puts("mapped_pages="); console_u64(p->space.count); console_putc('\n');
        for (size_t i = 0; i < p->space.count; i++) {
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

static int cmd_queues(size_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts("ticks="); console_u64(shell.scheduler->ticks);
        console_puts(" boost_interval="); console_u64(shell.scheduler->config.boost_interval);
        console_putc('\n');
        for (size_t i = 0; i < MLFQ_LEVELS; i++) {
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

static int cmd_spawn(size_t argc, char **argv) {
        enum job_kind kind;
        if (equal(argv[1], "cpu")) kind = JOB_CPU;
        else if (equal(argv[1], "yield")) kind = JOB_YIELD;
        else if (equal(argv[1], "sleep")) kind = JOB_SLEEP;
        else return PROC_EINVAL;
        uint64_t steps = 6;
        if (argc == 3 && number(argv[2], &steps) != 0) return PROC_EINVAL;
        if (steps == 0 || steps > JOB_MAX_STEPS) return PROC_EINVAL;
        return spawn(kind, steps);
}

static int cmd_run(size_t argc, char **argv) {
        (void)argc; (void)argv;
        int err = mlfq_run(shell.scheduler);
        if (err != 0) return err;
        console_puts("scheduler idle\n");
        return 0;
}

static int cmd_wake(size_t argc, char **argv) {
        (void)argc;
        int count = 0;
        if (equal(argv[1], "all")) {
                for (size_t i = 0; i < PROC_MAX; i++) {
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

static int cmd_kill(size_t argc, char **argv) {
        (void)argc;
        size_t count = 0;
        if (equal(argv[1], "all")) {
                for (size_t i = 0; i < PROC_MAX; i++) {
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

static int cmd_reap(size_t argc, char **argv) {
        (void)argc;
        size_t count = 0;
        if (equal(argv[1], "all")) {
                for (size_t i = 0; i < PROC_MAX; i++) {
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

static int cmd_exit(size_t argc, char **argv) {
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

static int cmd_boost(size_t argc, char **argv) {
        (void)argc; (void)argv;
        int err = mlfq_boost(shell.scheduler);
        if (err == 0) console_puts("priorities boosted\n");
        return err;
}

static int cmd_tick(size_t argc, char **argv) {
        uint64_t count = 1;
        if (argc == 2 && number(argv[1], &count) != 0) return PROC_EINVAL;
        if (count == 0 || count > 65536) return PROC_EINVAL;
        for (uint64_t i = 0; i < count; i++) {
                int err = mlfq_tick(shell.scheduler);
                if (err < 0) return err;
        }
        console_puts("ticks="); console_u64(shell.scheduler->ticks); console_putc('\n');
        return 0;
}

static int cmd_image(size_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = proc_create(shell.table, argv[1], &p);
        if (err != 0) return err;
        console_puts("image pid="); console_u64(p->pid);
        console_puts(" name="); console_puts(p->name); console_putc('\n');
        return 0;
}

static int cmd_fork(size_t argc, char **argv) {
        (void)argc;
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        int64_t pid = proc_fork(shell.table, p);
        if (pid < 0) return (int)pid;
        console_puts("fork child="); console_i64(pid);
        console_puts(" parent="); console_u64(p->pid); console_putc('\n');
        return 0;
}

static int cmd_exec(size_t argc, char **argv) {
        struct proc *p;
        int err = get_process(argv[1], &p);
        if (err != 0) return err;
        if (argc - 3 > PROC_ARG_MAX) return PROC_E2BIG;
        const char *args[PROC_ARG_MAX];
        for (size_t i = 3; i < argc; i++) args[i - 3] = argv[i];
        err = proc_exec(shell.table, p, argv[2], argc - 3, args);
        if (err != 0) return err;
        console_puts("exec pid="); console_u64(p->pid);
        console_puts(" name="); console_puts(p->name);
        console_puts(" argc="); console_u64(p->tf.x[PROC_A0]);
        console_puts(" entry="); console_hex(p->tf.pc); console_putc('\n');
        return 0;
}

static int cmd_demo(size_t argc, char **argv) {
        (void)argc; (void)argv;
        for (size_t i = 0; i < PROC_MAX; i++)
                if (shell.table->entries[i].pstatus != UNUSED) return PROC_EBUSY;
        for (size_t i = 0; i < 3; i++) {
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

static int cmd_quit(size_t argc, char **argv) {
        (void)argc; (void)argv;
        console_puts("bye\n");
        console_shutdown(0);
}

struct command {
        const char *name;
        size_t min_args, max_args;
        int (*run)(size_t argc, char **argv);
};
static const struct command commands[] = {
        {"help", 1, 1, cmd_help}, {"echo", 1, ARG_COUNT, cmd_echo},
        {"mem", 1, 1, cmd_mem}, {"alloc", 2, 2, cmd_alloc}, {"free", 2, 2, cmd_free},
        {"ps", 1, 1, cmd_ps}, {"show", 2, 2, cmd_show}, {"queues", 1, 1, cmd_queues},
        {"spawn", 2, 3, cmd_spawn}, {"run", 1, 1, cmd_run}, {"wake", 2, 2, cmd_wake},
        {"kill", 2, 2, cmd_kill}, {"reap", 2, 2, cmd_reap}, {"exit", 2, 3, cmd_exit},
        {"boost", 1, 1, cmd_boost}, {"tick", 1, 2, cmd_tick},
        {"image", 2, 2, cmd_image}, {"fork", 2, 2, cmd_fork}, {"exec", 3, 11, cmd_exec},
        {"demo", 1, 1, cmd_demo}, {"quit", 1, 1, cmd_quit},
};

static int execute(char *line) {
        char *args[ARG_COUNT];
        size_t count = 0;
        while (*line != '\0') {
                while (*line == ' ' || *line == '\t') line++;
                if (*line == '\0') break;
                if (count == ARG_COUNT) return PROC_E2BIG;
                args[count++] = line;
                while (*line != '\0' && *line != ' ' && *line != '\t') line++;
                if (*line != '\0') *line++ = '\0';
        }
        if (count == 0) return 0;
        for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
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
        console_puts("\nos_learn stage shell\n"
                     "Buddy memory + callable processes + cooperative MLFQ\n"
                     "Type help for commands, or demo for a scheduling walkthrough.\n");
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
