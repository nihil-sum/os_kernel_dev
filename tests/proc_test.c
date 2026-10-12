#include "proc.h"
#include "syscall.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGES 128u
static _Alignas(BUDDY_PAGE_SIZE) unsigned char memory[TEST_PAGES * BUDDY_PAGE_SIZE];
static struct buddy_page metadata[TEST_PAGES];
static struct buddy_allocator allocator;
static struct proc_table table;
static unsigned char code[BUDDY_PAGE_SIZE + 4];
static unsigned char data[BUDDY_PAGE_SIZE + 4];
static const unsigned char tiny_code[] = {0x73, 0, 0, 0};
static const struct proc_image images[] = {
        {"demo", code, sizeof(code), data, sizeof(data), 64, 2},
        {"tiny", tiny_code, sizeof(tiny_code), NULL, 0, 0, 0},
        {"bad-entry", tiny_code, sizeof(tiny_code), NULL, 0, 0, 1},
        {"bad-size", tiny_code, KSIZE_MAX, NULL, 0, 0, 0},
        {"bad-bss", tiny_code, sizeof(tiny_code), NULL, 0, KSIZE_MAX, 0},
};

static ksize_t free_pages(void) {
        ksize_t count = 0;
        for (int i = 0; i <= BUDDY_MAX_ORDER; i++)
                count += allocator.free_lists[i].len * ((ksize_t)1 << i);
        return count;
}

static ksize_t processes(void) {
        ksize_t count = 0;
        for (ksize_t i = 0; i < PROC_MAX; i++)
                count += table.entries[i].pstatus != UNUSED;
        return count;
}

static void fixture(void) {
        struct buddy_arena arena = {(uptr_t)memory, sizeof(memory)};
        assert(buddy_init(&allocator, &arena, metadata, TEST_PAGES) == BUDDY_OK);
        assert(proc_table_init(&table, &allocator, images,
                               sizeof(images) / sizeof(images[0])) == 0);
        for (ksize_t i = 0; i < sizeof(code); i++) code[i] = (unsigned char)(i * 7);
        for (ksize_t i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i * 11);
}

static void destroy(struct proc *p) {
        u64_t pid = p->pid;
        assert(proc_exit(&table, p, 42) == 0);
        if (table.current == p) assert(proc_deactivate(&table) == 0);
        assert(proc_reap(&table, pid) == 0);
}

static void allocator_checks(void) {
        fixture();
        struct block all, unchanged = {123, 4};
        assert(buddy_alloc(&allocator, 7, &all) == BUDDY_OK);
        assert(free_pages() == 0);
        struct block result = unchanged;
        assert(buddy_alloc(&allocator, 0, &result) == BUDDY_NO_MEMORY);
        assert(memcmp(&result, &unchanged, sizeof(result)) == 0);
        struct block wrong = {all.addr, 0};
        assert(buddy_free(&allocator, &wrong) == BUDDY_NOT_ALLOCATED);
        wrong = (struct block){all.addr + BUDDY_PAGE_SIZE, 0};
        assert(buddy_free(&allocator, &wrong) == BUDDY_NOT_ALLOCATED);
        assert(buddy_free(&allocator, &all) == BUDDY_OK);
        assert(buddy_free(&allocator, &all) == BUDDY_NOT_ALLOCATED);
        assert(buddy_alloc(&allocator, -1, &result) == BUDDY_INVALID_ORDER);
        assert(buddy_alloc(&allocator, BUDDY_MAX_ORDER + 1, &result) == BUDDY_INVALID_ORDER);
        struct block pages[TEST_PAGES];
        for (ksize_t i = 0; i < TEST_PAGES; i++) {
                assert(buddy_alloc(&allocator, 0, &pages[i]) == BUDDY_OK);
                assert(buddy_page_at(&allocator, pages[i].addr)->state == BUDDY_PAGE_ALLOCATED);
                for (ksize_t j = 0; j < i; j++) assert(pages[i].addr != pages[j].addr);
        }
        for (ksize_t i = 0; i < TEST_PAGES; i += 2)
                assert(buddy_free(&allocator, &pages[i]) == BUDDY_OK);
        for (ksize_t i = 1; i < TEST_PAGES; i += 2)
                assert(buddy_free(&allocator, &pages[i]) == BUDDY_OK);
        assert(allocator.free_lists[7].len == 1 && free_pages() == TEST_PAGES);

        /* Non-power-of-two arena, with order sizes aligned relative to base. */
        struct buddy_arena odd = {(uptr_t)memory + BUDDY_PAGE_SIZE,
                                 13 * BUDDY_PAGE_SIZE};
        assert(buddy_init(&allocator, &odd, metadata, TEST_PAGES) == BUDDY_OK);
        for (ksize_t i = 0; i < 13; i++) assert(buddy_alloc(&allocator, 0, &pages[i]) == BUDDY_OK);
        assert(buddy_alloc(&allocator, 0, &result) == BUDDY_NO_MEMORY);
        for (ksize_t i = 13; i > 0; i--) assert(buddy_free(&allocator, &pages[i - 1]) == BUDDY_OK);
        assert(free_pages() == 13);
        assert(allocator.free_lists[3].len == 1 && allocator.free_lists[2].len == 1 &&
               allocator.free_lists[0].len == 1);
}

static void basic_checks(void) {
        fixture();
        struct proc *p;
        assert(proc_create(&table, "demo", &p) == 0);
        assert(p->pid == 1 && p->pstatus == RUNNABLE && p->space.count == 5);
        assert(p->tf.pc == PROC_CODE_BASE + 2 && p->tf.x[PROC_SP] % 16 == 0);
        unsigned char copy[sizeof(code)];
        assert(proc_copyin(p, copy, PROC_CODE_BASE, sizeof(copy)) == 0);
        assert(memcmp(code, copy, sizeof(copy)) == 0);
        u64_t data_va = PROC_CODE_BASE + 2 * BUDDY_PAGE_SIZE;
        assert(proc_copyin(p, copy, data_va, sizeof(data)) == 0);
        assert(memcmp(data, copy, sizeof(data)) == 0);
        unsigned char bss[64];
        assert(proc_copyin(p, bss, data_va + sizeof(data), sizeof(bss)) == 0);
        for (ksize_t i = 0; i < sizeof(bss); i++) assert(bss[i] == 0);
        assert(proc_copyout(p, PROC_CODE_BASE, "x", 1) == PROC_EFAULT);
        assert(proc_copyout(p, PROC_STACK_BASE - 1, "xx", 2) == PROC_EFAULT);
        assert(proc_copyin(p, copy, U64_MAX, 2) == PROC_EFAULT);
        assert(proc_copyin(p, copy, 0, 1) == PROC_EFAULT);
        /* A cross-page copy whose final page is unmapped must write nothing. */
        unsigned char byte;
        u64_t end = data_va + 2 * BUDDY_PAGE_SIZE - 1;
        assert(proc_copyin(p, &byte, end, 1) == 0);
        assert(proc_copyout(p, end, "xy", 2) == PROC_EFAULT);
        unsigned char after;
        assert(proc_copyin(p, &after, end, 1) == 0 && after == byte);
        assert(proc_copyout(p, data_va + BUDDY_PAGE_SIZE - 2, "hello", 6) == 0);
        char text[6];
        assert(proc_copyinstr(p, text, data_va + BUDDY_PAGE_SIZE - 2, sizeof(text)) == 0);
        assert(strcmp(text, "hello") == 0);

        p->tf.x[5] = 0x123456;
        assert(proc_activate(&table, p) == 0);
        p->tf.x[PROC_A7] = SYS_FORK;
        u64_t pc = p->tf.pc;
        assert(syscall_dispatch(&table) == SYSCALL_RETURN);
        struct proc *child = proc_find(&table, p->tf.x[PROC_A0]);
        assert(child != NULL && child->parent == p && child->pstatus == RUNNABLE);
        assert(child->tf.pc == pc + 4 && p->tf.pc == pc + 4);
        assert(child->tf.x[PROC_A0] == 0 && child->tf.x[5] == 0x123456);
        assert(child->kstack.addr != p->kstack.addr && child->ctx.ra == 0);
        for (ksize_t i = 0; i < p->space.count; i++) {
                assert(child->space.pages[i].backing.addr != p->space.pages[i].backing.addr);
                assert(child->space.pages[i].va == p->space.pages[i].va);
                assert(memcmp((void *)child->space.pages[i].backing.addr,
                              (void *)p->space.pages[i].backing.addr, BUDDY_PAGE_SIZE) == 0);
        }
        assert(proc_copyout(child, data_va, "z", 1) == 0);
        assert(proc_copyin(p, &byte, data_va, 1) == 0 && byte == data[0]);
        assert(proc_copyout(p, data_va, "q", 1) == 0);
        assert(proc_copyin(child, &byte, data_va, 1) == 0 && byte == 'z');

        /* User name and argv lie in the old image and are copied before exec. */
        u64_t name_va = PROC_STACK_BASE + 16;
        u64_t arg_va = PROC_STACK_BASE + 64;
        u64_t argv_va = PROC_STACK_BASE + 128;
        u64_t args[] = {arg_va, 0};
        assert(proc_copyout(p, name_va, "tiny", 5) == 0);
        assert(proc_copyout(p, arg_va, "argument", 9) == 0);
        assert(proc_copyout(p, argv_va, args, sizeof(args)) == 0);
        u64_t pid = p->pid;
        struct block kstack = p->kstack;
        p->tf.x[PROC_A7] = SYS_EXEC;
        p->tf.x[PROC_A0] = name_va;
        p->tf.x[PROC_A1] = argv_va;
        assert(syscall_dispatch(&table) == SYSCALL_EXEC);
        assert(p->pid == pid && p->kstack.addr == kstack.addr && p->pstatus == RUNNING);
        assert(p->tf.pc == PROC_CODE_BASE && p->tf.x[PROC_A0] == 1);
        assert(p->tf.x[PROC_SP] % 16 == 0 && strcmp(p->name, "tiny") == 0);
        assert(p->tf.x[5] == 0 && p->space.count == 2);
        assert(proc_copyin(p, args, p->tf.x[PROC_A1], sizeof(args)) == 0);
        assert(args[1] == 0);
        char argument[16];
        assert(proc_copyinstr(p, argument, args[0], sizeof(argument)) == 0);
        assert(strcmp(argument, "argument") == 0);
        assert(proc_copyin(p, &byte, data_va, 1) == PROC_EFAULT);
        child->pstatus = SLEEPING;
        p->tf.x[PROC_A7] = SYS_KILL;
        p->tf.x[PROC_A0] = child->pid;
        assert(syscall_dispatch(&table) == SYSCALL_RETURN);
        assert(p->tf.x[PROC_A0] == 0 && child->killed && child->pstatus == RUNNABLE);
        assert(proc_kill(&table, child->pid) == 0);
        assert(proc_deactivate(&table) == 0);
        assert(proc_activate(&table, child) == PROC_ESRCH);
        assert(child->pstatus == ZOMBIE && child->exit_status == -1);
        assert(proc_kill(&table, child->pid) == PROC_ESRCH);
        assert(proc_reap(&table, child->pid) == 0);
        assert(proc_activate(&table, p) == 0);
        p->tf.x[PROC_A7] = SYS_KILL;
        p->tf.x[PROC_A0] = p->pid;
        ksize_t before = free_pages();
        assert(syscall_dispatch(&table) == SYSCALL_EXIT);
        assert(p->pstatus == ZOMBIE && free_pages() == before);
        assert(proc_reap(&table, p->pid) == PROC_EBUSY);
        pid = p->pid;
        assert(proc_deactivate(&table) == 0);
        assert(proc_reap(&table, pid) == 0);
        assert(free_pages() == TEST_PAGES && processes() == 0);
}

static void invalid_checks(void) {
        fixture();
        struct proc *p, *unchanged = NULL;
        assert(proc_create(&table, "missing", &unchanged) == PROC_ENOEXEC);
        assert(unchanged == NULL && free_pages() == TEST_PAGES);
        assert(proc_create(&table, "demo", &p) == 0);
        struct proc snapshot = *p;
        const char *bad[] = {NULL};
        assert(proc_exec(&table, p, "tiny", 1, bad) == PROC_E2BIG);
        assert(proc_exec(&table, p, "tiny", PROC_ARG_MAX + 1, NULL) == PROC_E2BIG);
        assert(proc_exec(&table, p, "bad-entry", 0, NULL) == PROC_ENOEXEC);
        assert(proc_exec(&table, p, "bad-size", 0, NULL) == PROC_ENOEXEC);
        assert(proc_exec(&table, p, "bad-bss", 0, NULL) == PROC_ENOEXEC);
        assert(proc_exec(&table, p, "missing", 0, NULL) == PROC_ENOEXEC);
        assert(memcmp(p, &snapshot, sizeof(snapshot)) == 0);
        assert(proc_reap(&table, p->pid) == PROC_EBUSY);
        assert(proc_kill(&table, U64_MAX) == PROC_ESRCH);
        assert(proc_kill(&table, 0) == PROC_ESRCH);
        assert(sys_fork(&table) == PROC_EINVAL);
        assert(proc_activate(&table, p) == 0);
        assert(proc_activate(&table, p) == PROC_EBUSY);
        assert(sys_exec(&table, 0, 0) == PROC_EFAULT);
        u64_t va = PROC_STACK_BASE;
        assert(proc_copyout(p, va, "tiny", 5) == 0);
        assert(sys_exec(&table, va, U64_MAX) == PROC_EFAULT);
        u64_t user_args[PROC_ARG_MAX + 1];
        for (ksize_t i = 0; i < PROC_ARG_MAX + 1; i++) user_args[i] = va;
        assert(proc_copyout(p, va + 64, user_args, sizeof(user_args)) == 0);
        assert(sys_exec(&table, va, va + 64) == PROC_E2BIG);
        unsigned char unterminated[PROC_ARG_BYTES];
        memset(unterminated, 'a', sizeof(unterminated));
        assert(proc_copyout(p, va + 256, unterminated, sizeof(unterminated)) == 0);
        user_args[0] = va + 256;
        user_args[1] = 0;
        assert(proc_copyout(p, va + 64, user_args, 2 * sizeof(u64_t)) == 0);
        assert(sys_exec(&table, va, va + 64) == PROC_E2BIG);
        assert(sys_exec(&table, PROC_STACK_TOP - 1, 0) == PROC_EINVAL);
        /* The name itself runs across an unmapped boundary. */
        assert(proc_copyout(p, PROC_STACK_TOP - 1, "x", 1) == 0);
        assert(sys_exec(&table, PROC_STACK_TOP - 1, 0) == PROC_EFAULT);
        p->tf.x[PROC_A7] = 999;
        assert(syscall_dispatch(&table) == SYSCALL_RETURN);
        assert((i64_t)p->tf.x[PROC_A0] == PROC_ENOSYS);
        p->tf.x[PROC_A7] = SYS_EXEC;
        p->tf.x[PROC_A0] = 0;
        u64_t pc = p->tf.pc;
        assert(syscall_dispatch(&table) == SYSCALL_RETURN);
        assert(p->tf.pc == pc + 4 && (i64_t)p->tf.x[PROC_A0] == PROC_EFAULT);
        destroy(p);
        assert(free_pages() == TEST_PAGES);
}

static void exhaustion_checks(void) {
        /* Fail at every allocation point for create, fork, and exec. */
        for (int op = 0; op < 3; op++) {
                ksize_t allocations = op == 2 ? 5 : 6;
                for (ksize_t available = 0; available < allocations; available++) {
                        fixture();
                        struct proc *p = NULL;
                        if (op != 0) assert(proc_create(&table, "demo", &p) == 0);
                        struct proc snapshot;
                        if (p != NULL) snapshot = *p;
                        struct block held[TEST_PAGES];
                        ksize_t count = 0;
                        while (free_pages() > available)
                                assert(buddy_alloc(&allocator, 0, &held[count++]) == BUDDY_OK);
                        ksize_t before = free_pages();
                        if (op == 0) {
                                struct proc *out = NULL;
                                assert(proc_create(&table, "demo", &out) == PROC_ENOMEM);
                                assert(out == NULL && processes() == 0);
                        } else if (op == 1) {
                                assert(proc_fork(&table, p) == PROC_ENOMEM);
                        } else {
                                assert(proc_exec(&table, p, "demo", 0, NULL) == PROC_ENOMEM);
                        }
                        assert(free_pages() == before);
                        if (p != NULL) {
                                assert(processes() == 1 && memcmp(p, &snapshot, sizeof(snapshot)) == 0);
                                unsigned char byte;
                                assert(proc_copyin(p, &byte, PROC_CODE_BASE, 1) == 0 && byte == code[0]);
                        }
                        for (ksize_t i = 0; i < count; i++)
                                assert(buddy_free(&allocator, &held[i]) == BUDDY_OK);
                        if (p != NULL) destroy(p);
                        assert(free_pages() == TEST_PAGES && allocator.free_lists[7].len == 1);
                }
        }
        fixture();
        struct proc *all[PROC_MAX];
        for (ksize_t i = 0; i < PROC_MAX; i++) assert(proc_create(&table, "tiny", &all[i]) == 0);
        ksize_t before = free_pages();
        struct proc *out = NULL;
        assert(proc_create(&table, "tiny", &out) == PROC_ENOMEM);
        assert(proc_fork(&table, all[0]) == PROC_ENOMEM && free_pages() == before);
        u64_t previous_pid = all[PROC_MAX - 1]->pid;
        destroy(all[PROC_MAX - 1]);
        assert(proc_create(&table, "tiny", &all[PROC_MAX - 1]) == 0);
        assert(all[PROC_MAX - 1]->pid > previous_pid);
        for (ksize_t i = 0; i < PROC_MAX; i++) destroy(all[i]);
        assert(free_pages() == TEST_PAGES);
}

static void lifecycle_checks(void) {
        fixture();
        struct proc *parent;
        assert(proc_create(&table, "tiny", &parent) == 0);
        i64_t pid = proc_fork(&table, parent);
        struct proc *child = proc_find(&table, (u64_t)pid);
        assert(child != NULL);
        destroy(parent);
        assert(child->parent == NULL);
        assert(proc_activate(&table, child) == 0);
        assert(proc_kill(&table, child->pid) == 0);
        assert(syscall_dispatch(&table) == SYSCALL_EXIT);
        pid = (i64_t)child->pid;
        assert(proc_deactivate(&table) == 0);
        assert(proc_reap(&table, (u64_t)pid) == 0);
        assert(proc_find(&table, (u64_t)pid) == NULL);
        assert(proc_reap(&table, (u64_t)pid) == PROC_ESRCH);
        assert(free_pages() == TEST_PAGES);
        assert(proc_create(&table, "tiny", &parent) == 0);
        assert(proc_activate(&table, parent) == 0);
        parent->tf.pc = U64_MAX;
        assert(syscall_dispatch(&table) == SYSCALL_EXIT);
        pid = (i64_t)parent->pid;
        assert(proc_deactivate(&table) == 0);
        assert(proc_reap(&table, (u64_t)pid) == 0);
        assert(free_pages() == TEST_PAGES);
}

struct kernel_context {
        struct proc *proc;
        unsigned int calls;
        int kill_self;
        int explicit_exit;
};

static i32_t kernel_entry(void *arg) {
        struct kernel_context *ctx = arg;
        ctx->calls++;
        assert(table.current == ctx->proc && ctx->proc->entry_active);
        assert(proc_run_callable(&table) == PROC_EBUSY);
        assert(proc_deactivate(&table) == PROC_EBUSY);
        assert(proc_reap(&table, ctx->proc->pid) == PROC_EBUSY);
        if (ctx->kill_self) assert(proc_kill(&table, ctx->proc->pid) == 0);
        if (ctx->explicit_exit) {
                assert(proc_exit(&table, ctx->proc, 77) == 0);
                assert(proc_deactivate(&table) == PROC_EBUSY);
                assert(proc_reap(&table, ctx->proc->pid) == PROC_EBUSY);
        }
        return -23;
}

static u64_t kernel_wide_entry(void) { return U64_MAX; }
static void *kernel_pointer_entry(void *arg) { return arg; }
static void kernel_void_entry(void *arg) { ++*(unsigned int *)arg; }

static void reap_current_kernel(struct proc *p) {
        u64_t pid = p->pid;
        assert(p->pstatus == ZOMBIE && !p->entry_active);
        assert(proc_run_callable(&table) == PROC_EBUSY);
        assert(proc_reap(&table, pid) == PROC_EBUSY);
        assert(proc_deactivate(&table) == 0);
        assert(proc_reap(&table, pid) == 0);
        assert(free_pages() == TEST_PAGES);
}

static void kernel_checks(void) {
        fixture();
        /* Callable processes also work with no flat-image registry at all. */
        assert(proc_table_init(&table, &allocator, NULL, 0) == 0);
        struct proc *p = NULL;
        struct kernel_context ctx = {0};
        struct callable entry = callable_from_fni32p(kernel_entry, &ctx);
        assert(proc_create_callable(&table, "worker", &entry, &p) == 0);
        ctx.proc = p;
        assert(p->kind == PROC_KERNEL && p->pstatus == RUNNABLE);
        assert(p->space.count == 0 && p->tf.pc == 0 && p->result.tag == 0);
        assert(free_pages() == TEST_PAGES - 1 && p->ctx.ra == 0);
        entry = (struct callable){0}; /* Descriptor was copied at creation. */
        assert(proc_fork(&table, p) == PROC_ENOSYS);
        assert(proc_exec(&table, p, "tiny", 0, NULL) == PROC_ENOEXEC);
        assert(proc_run_callable(&table) == PROC_EINVAL);
        assert(proc_activate(&table, p) == 0);
        assert(sys_fork(&table) == PROC_EINVAL);
        assert(sys_exec(&table, 0, 0) == PROC_EINVAL);
        assert(sys_kill(&table, p->pid) == PROC_EINVAL);
        assert(proc_run_callable(&table) == 0);
        assert(ctx.calls == 1 && p->entry_started && p->exit_status == -23);
        assert(p->result.tag == fi32p && p->result.value.i32 == -23);
        reap_current_kernel(p);

        unsigned int calls = 0;
        struct callable entries[] = {
                callable_from_fnu64v(kernel_wide_entry),
                callable_from_fnptrp(kernel_pointer_entry, &calls),
                callable_from_fnv(kernel_void_entry, &calls),
        };
        for (ksize_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
                assert(proc_create_callable(&table, "typed-worker", &entries[i], &p) == 0);
                assert(proc_activate(&table, p) == 0 && proc_run_callable(&table) == 0);
                assert(p->result.tag == entries[i].tag && p->exit_status == 0);
                if (i == 0) assert(p->result.value.u64 == U64_MAX);
                if (i == 1) assert(p->result.value.ptr == &calls);
                if (i == 2) assert(calls == 1);
                reap_current_kernel(p);
        }

        /* Invalid callables are rejected before allocating a PID or stack. */
        p = NULL;
        u64_t next = table.next_pid;
        assert(proc_create_callable(&table, "invalid", &entry, &p) == PROC_EINVAL);
        entry = callable_from_fni32p(NULL, &ctx);
        assert(proc_create_callable(&table, "invalid", &entry, &p) == PROC_EINVAL);
        entry.tag = (CallableType)999;
        assert(proc_create_callable(&table, "invalid", &entry, &p) == PROC_EINVAL);
        assert(p == NULL && table.next_pid == next && free_pages() == TEST_PAGES);

        entry = callable_from_fni32p(kernel_entry, &ctx);
        ctx.calls = 0;
        assert(proc_create_callable(&table, "cancel-before-run", &entry, &p) == 0);
        assert(proc_kill(&table, p->pid) == 0);
        assert(proc_activate(&table, p) == PROC_ESRCH);
        assert(p->pstatus == ZOMBIE && !p->entry_started && ctx.calls == 0);
        assert(proc_reap(&table, p->pid) == 0);

        assert(proc_create_callable(&table, "cancel-after-activate", &entry, &p) == 0);
        assert(proc_activate(&table, p) == 0);
        assert(proc_kill(&table, p->pid) == 0 && proc_run_callable(&table) == 0);
        assert(!p->entry_started && p->result.tag == 0 && ctx.calls == 0);
        u64_t pid = p->pid;
        assert(proc_deactivate(&table) == 0 && proc_reap(&table, pid) == 0);

        for (int mode = 0; mode < 3; mode++) {
                ctx = (struct kernel_context){ .kill_self = mode != 1,
                                               .explicit_exit = mode != 0 };
                assert(proc_create_callable(&table, "exit-worker", &entry, &p) == 0);
                ctx.proc = p;
                assert(proc_activate(&table, p) == 0 && proc_run_callable(&table) == 0);
                assert(ctx.calls == 1 && p->exit_status == (mode == 1 ? 77 : -1));
                reap_current_kernel(p);
        }

        struct block held[TEST_PAGES];
        for (ksize_t i = 0; i < TEST_PAGES; i++)
                assert(buddy_alloc(&allocator, 0, &held[i]) == BUDDY_OK);
        p = NULL;
        assert(proc_create_callable(&table, "out-of-memory", &entry, &p) == PROC_ENOMEM);
        assert(p == NULL && processes() == 0);
        for (ksize_t i = 0; i < TEST_PAGES; i++)
                assert(buddy_free(&allocator, &held[i]) == BUDDY_OK);
        assert(free_pages() == TEST_PAGES);
        fixture();
        assert(proc_create(&table, "tiny", &p) == 0 && proc_activate(&table, p) == 0);
        assert(proc_run_callable(&table) == PROC_EINVAL);
        destroy(p);
}

int main(void) {
        allocator_checks();
        basic_checks();
        invalid_checks();
        exhaustion_checks();
        lifecycle_checks();
        kernel_checks();
        puts("PROC OK: buddy allocation, fork, exec, syscall validation, kill, rollback, reap, callable processes");
        return 0;
}
