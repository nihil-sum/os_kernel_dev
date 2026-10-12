#include "mlfq.h"
#include "syscall.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PAGES 128u
static _Alignas(BUDDY_PAGE_SIZE) unsigned char memory[TEST_PAGES * BUDDY_PAGE_SIZE];
static struct buddy_page pages[TEST_PAGES];
static struct buddy_allocator allocator;
static struct proc_table table;
static struct mlfq scheduler;
static const unsigned char code[] = {0x73, 0, 0, 0};
static const struct proc_image images[] = {{"test", code, sizeof(code), NULL, 0, 0, 0}};
static int channel;

static void fixture(u64_t boost) {
        struct buddy_arena arena = {(uptr_t)memory, sizeof(memory)};
        assert(buddy_init(&allocator, &arena, pages, TEST_PAGES) == BUDDY_OK);
        assert(proc_table_init(&table, &allocator, images, 1) == 0);
        struct mlfq_config config = mlfq_default_config;
        config.boost_interval = boost;
        assert(mlfq_init(&scheduler, &table, &config) == 0);
}

static struct proc *create(void) {
        struct proc *p;
        assert(proc_create(&table, "test", &p) == 0);
        assert(p->schedule.owner == &scheduler && p->schedule.queued);
        return p;
}

static struct proc *dispatch(void) {
        struct proc *p = NULL;
        assert(mlfq_dispatch(&scheduler, &p) == 1 && p == table.current);
        assert(p->pstatus == RUNNING && !p->schedule.queued);
        return p;
}

static void check_queues(void) {
        ksize_t total = 0;
        for (ksize_t level = 0; level < MLFQ_LEVELS; level++) {
                const struct list_meta *queue = &scheduler.queues[level];
                ksize_t count = 0;
                struct node *prev = NULL;
                for (struct node *node = queue->head; node != NULL; node = node->next) {
                        assert(++count <= PROC_MAX && node->prev == prev);
                        struct proc *p = node->data;
                        assert(p->pstatus == RUNNABLE && p->schedule.queued);
                        assert(p->schedule.owner == &scheduler && p->schedule.level == level);
                        assert(node == &p->schedule.ready_node && p != table.current);
                        prev = node;
                }
                assert(count == queue->len && prev == queue->tail);
                total += count;
        }
        ksize_t marked = 0;
        for (ksize_t i = 0; i < PROC_MAX; i++) {
                const struct proc *p = &table.entries[i];
                if (p->schedule.owner == &scheduler && p->pstatus == RUNNABLE)
                        assert(p->schedule.queued);
                marked += p->schedule.queued != 0;
        }
        assert(total == marked);
}

static void cleanup(void) {
        if (table.current != NULL) {
                assert(proc_exit(&table, table.current, 0) == 0);
                assert(mlfq_complete(&scheduler, MLFQ_EXIT, NULL) == 0);
        }
        for (ksize_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &table.entries[i];
                if (p->pstatus == UNUSED) continue;
                if (p->pstatus != ZOMBIE) assert(proc_exit(&table, p, 0) == 0);
                assert(proc_reap(&table, p->pid) == 0);
        }
        check_queues();
        assert(allocator.free_lists[7].len == 1);
        assert(mlfq_destroy(&scheduler) == 0 && table.scheduler == NULL);
}

static void round_robin_and_demotion(void) {
        fixture(0);
        struct proc *a = create(), *b = create();
        assert(dispatch() == a);
        assert(mlfq_tick(&scheduler) == 1);
        assert(a->schedule.slice_left == 0 && a->schedule.budget_left == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(a->schedule.level == 0 && a->schedule.slice_left == 1);
        assert(dispatch() == b);
        assert(mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(dispatch() == a);
        assert(mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(a->schedule.level == 1 && a->schedule.slice_left == 2 && a->schedule.budget_left == 4);
        assert(dispatch() == b); /* Higher-priority B wins over demoted A. */
        assert(proc_exit(&table, b, 0) == 0 && mlfq_complete(&scheduler, MLFQ_EXIT, NULL) == 0);
        for (unsigned int expected = 2; expected <= MLFQ_LEVELS; expected++) {
                unsigned int level = expected - 1;
                u64_t budget = scheduler.config.allotment[level];
                while (budget > 0) {
                        assert(dispatch() == a);
                        u64_t slice = a->schedule.slice_left;
                        for (u64_t i = 0; i < slice; i++) {
                                int request = mlfq_tick(&scheduler);
                                assert(request == (i + 1 == slice));
                        }
                        budget -= slice;
                        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
                }
                assert(a->schedule.level == (expected < MLFQ_LEVELS ? expected : MLFQ_LEVELS - 1));
        }
        check_queues();
        cleanup();
}

static void yield_sleep_and_priority(void) {
        fixture(0);
        struct proc *a = create();
        assert(dispatch() == a);
        assert(mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == 0);
        assert(dispatch() == a);
        assert(mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(a->schedule.level == 1 && a->pstatus == SLEEPING);
        assert(mlfq_destroy(&scheduler) == PROC_EBUSY);
        assert(mlfq_wakeup(&scheduler, &channel) == 1);
        assert(dispatch() == a && mlfq_tick(&scheduler) == 0);
        assert(a->schedule.slice_left == 1 && a->schedule.budget_left == 3);
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == 0);
        assert(dispatch() == a);
        assert(a->schedule.slice_left == 1 && a->schedule.budget_left == 3);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(mlfq_wakeup(&scheduler, &channel) == 1 && mlfq_wakeup(&scheduler, &channel) == 0);
        assert(dispatch() == a);
        struct proc *b = create();
        assert(mlfq_should_reschedule(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(a->schedule.slice_left == 1 && a->schedule.budget_left == 3);
        assert(dispatch() == b);
        assert(proc_exit(&table, b, 0) == 0 && mlfq_complete(&scheduler, MLFQ_EXIT, NULL) == 0);
        assert(dispatch() == a);
        assert(mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(a->schedule.level == 1 && a->schedule.budget_left == 2);
        assert(mlfq_wakeup(&scheduler, &channel) == 1);
        assert(dispatch() == a && a->schedule.slice_left == 2);
        check_queues();
        cleanup();
}

static void boosts(void) {
        fixture(5);
        struct proc *a = create();
        assert(dispatch() == a && mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(dispatch() == a && mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(a->schedule.level == 1 && a->pstatus == SLEEPING);
        struct proc *b = create();
        assert(dispatch() == b && mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(dispatch() == b && mlfq_tick(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(b->schedule.level == 1);
        struct proc *c = create();
        assert(dispatch() == c && mlfq_tick(&scheduler) == 1); /* Tick 5 boosts all. */
        assert(scheduler.ticks == 5 && scheduler.boost_elapsed == 0);
        assert(a->schedule.level == 0 && a->pstatus == SLEEPING);
        assert(b->schedule.level == 0 && b->schedule.queued);
        assert(c->schedule.budget_left == 2 && c->schedule.slice_left == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(dispatch() == b); /* Boosted waiting process goes before current. */
        assert(mlfq_wakeup(&scheduler, &channel) == 1);
        check_queues();
        cleanup();
        fixture(1);
        a = create();
        assert(dispatch() == a && mlfq_tick(&scheduler) == 1);
        assert(a->schedule.budget_left == 2); /* Boost wins over exhaustion. */
        cleanup();
        fixture(0);
        scheduler.ticks = U64_MAX;
        assert(mlfq_tick(&scheduler) == 0 && scheduler.ticks == U64_MAX);
        cleanup();
}

static void lifecycle(void) {
        fixture(0);
        struct proc *a = create();
        assert(dispatch() == a);
        a->tf.x[PROC_A7] = SYS_FORK;
        assert(syscall_dispatch(&table) == SYSCALL_RETURN);
        struct proc *child = proc_find(&table, a->tf.x[PROC_A0]);
        assert(child != NULL && child->schedule.queued && child->schedule.level == 0);
        assert(mlfq_tick(&scheduler) == 1);
        u64_t budget = a->schedule.budget_left;
        assert(proc_exec(&table, a, "test", 0, NULL) == 0);
        assert(a->schedule.budget_left == budget); /* Exec cannot reset CPU usage. */
        assert(proc_kill(&table, child->pid) == 0 && scheduler.queues[0].len == 1);
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == 0);
        assert(dispatch() == a); /* Dispatch discards the killed queued child. */
        assert(child->pstatus == ZOMBIE && !child->schedule.queued);
        struct proc *old_slot = child;
        u64_t old_pid = child->pid;
        assert(proc_reap(&table, old_pid) == 0);
        child = create();
        assert(child == old_slot && child->pid != old_pid);
        assert(proc_kill(&table, a->pid) == 0 && mlfq_should_reschedule(&scheduler) == 1);
        assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        assert(a->pstatus == ZOMBIE);
        assert(dispatch() == child);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(proc_kill(&table, child->pid) == 0 && child->pstatus == RUNNABLE);
        struct proc *untouched = a;
        assert(mlfq_dispatch(&scheduler, &untouched) == 0 && untouched == a);
        assert(child->pstatus == ZOMBIE);
        check_queues();
        cleanup();
}

static void manual_boost_and_multiple_wakeup(void) {
        fixture(0);
        struct proc *a = create();
        for (ksize_t i = 0; i < 2; i++) {
                assert(dispatch() == a && mlfq_tick(&scheduler) == 1);
                assert(mlfq_complete(&scheduler, MLFQ_PREEMPT, NULL) == 0);
        }
        assert(a->schedule.level == 1);
        struct proc *b = create(), *c = create();
        assert(mlfq_boost(&scheduler) == 0 && a->schedule.level == 0);
        check_queues();
        assert(dispatch() == b); /* Existing level-0 FIFO precedes boosted A. */
        assert(mlfq_boost(&scheduler) == 0);
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == 0);
        assert(dispatch() == c);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(dispatch() == a);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, &channel) == 0);
        assert(mlfq_wakeup(&scheduler, &channel) == 2);
        assert(mlfq_wakeup(&scheduler, &channel) == 0);
        check_queues();
        cleanup();
}

static void invalid_and_attach(void) {
        fixture(0);
        struct proc *p = create();
        assert(proc_activate(&table, p) == PROC_EBUSY);
        assert(mlfq_dispatch(&scheduler, NULL) == PROC_EINVAL);
        assert(mlfq_wakeup(&scheduler, NULL) == PROC_EINVAL);
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == PROC_EINVAL);
        struct mlfq second;
        assert(mlfq_init(&second, &table, NULL) == PROC_EBUSY);
        assert(dispatch() == p);
        assert(proc_deactivate(&table) == PROC_EBUSY);
        assert(mlfq_destroy(&scheduler) == PROC_EBUSY);
        assert(mlfq_dispatch(&scheduler, &p) == PROC_EBUSY);
        assert(mlfq_complete(&scheduler, MLFQ_EXIT, NULL) == PROC_EINVAL);
        assert(mlfq_complete(&scheduler, MLFQ_BLOCK, NULL) == PROC_EINVAL);
        assert(mlfq_complete(&scheduler, (enum mlfq_stop)99, NULL) == PROC_EINVAL);
        assert(table.current == p && p->pstatus == RUNNING);
        scheduler.in_process = 1;
        assert(mlfq_complete(&scheduler, MLFQ_YIELD, NULL) == PROC_EBUSY);
        scheduler.in_process = 0;
        cleanup();
        assert(mlfq_tick(&scheduler) == PROC_EINVAL);
        assert(mlfq_destroy(&scheduler) == PROC_EINVAL);
        struct mlfq_config bad = mlfq_default_config;
        bad.quantum[0] = 0;
        memset(&second, 0x5a, sizeof(second));
        struct mlfq snapshot = second;
        assert(mlfq_init(&second, &table, &bad) == PROC_EINVAL);
        assert(memcmp(&second, &snapshot, sizeof(second)) == 0 && table.scheduler == NULL);
        bad = mlfq_default_config;
        bad.allotment[1] = bad.quantum[1] - 1;
        assert(mlfq_init(&second, &table, &bad) == PROC_EINVAL);
        assert(proc_create(&table, "test", &p) == 0);
        p->pstatus = SLEEPING;
        assert(mlfq_init(&scheduler, &table, NULL) == 0);
        assert(p->schedule.owner == &scheduler && !p->schedule.queued);
        assert(proc_kill(&table, p->pid) == 0 && p->schedule.queued);
        cleanup();
}

int main(void) {
        round_robin_and_demotion();
        yield_sleep_and_priority();
        boosts();
        lifecycle();
        manual_boost_and_multiple_wakeup();
        invalid_and_attach();
        puts("MLFQ OK: priority, round robin, CPU budgets, sleep/wakeup, boosts, lifecycle");
        return 0;
}
