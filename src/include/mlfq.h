#ifndef MLFQ_H
#define MLFQ_H

#include "proc.h"

#define MLFQ_LEVELS 4u
/* Compatibility with the original scheduler draft. */
#define MAX_LEVEL MLFQ_LEVELS

struct mlfq_config {
        u64_t quantum[MLFQ_LEVELS];
        u64_t allotment[MLFQ_LEVELS];
        u64_t boost_interval; /* 0 disables periodic boosts. */
};

enum mlfq_stop { MLFQ_YIELD, MLFQ_PREEMPT, MLFQ_BLOCK, MLFQ_EXIT };

/* Single hart; caller serializes all process/scheduler operations. Storage
 * must remain stable, especially while a kernel continuation is suspended.
 * Level 0 is highest priority. Queues borrow embedded proc scheduling nodes.
 */
struct mlfq {
        struct proc_table *table;
        struct mlfq_config config;
        struct list_meta queues[MLFQ_LEVELS];
        u64_t ticks;
        u64_t boost_elapsed;
        int reschedule;
        struct context dispatcher;
        enum mlfq_stop pending;
        const void *pending_channel;
        int driver_active;
        int in_process;
};

extern const struct mlfq_config mlfq_default_config;
/* Attach to an initialized, idle table. Admits existing runnable/sleeping
 * processes; future create/fork/kill/exit operations notify it automatically.
 * NULL config selects defaults. Invalid configuration leaves inputs intact.
 */
int mlfq_init(struct mlfq *scheduler, struct proc_table *table,
              const struct mlfq_config *config);
/* Refuses an active driver/current process, suspended entries, or sleepers. */
int mlfq_destroy(struct mlfq *scheduler);

/* Policy boundary: 1 selects/activates highest-priority runnable proc, 0 is
 * idle, negative is error. Output untouched unless a process is selected.
 * mlfq_complete must run AFTER switching off the process stack. It releases
 * current and either requeues, sleeps, or leaves a zombie for proc_reap.
 */
int mlfq_dispatch(struct mlfq *scheduler, struct proc **out);
int mlfq_complete(struct mlfq *scheduler, enum mlfq_stop reason,
                  const void *channel);
/* Charge one CPU tick, advance the boost clock, return reschedule request
 * (1/0), or negative error. Does not itself perform a context switch.
 */
int mlfq_tick(struct mlfq *scheduler);
int mlfq_should_reschedule(const struct mlfq *scheduler);
int mlfq_boost(struct mlfq *scheduler);
int mlfq_wakeup(struct mlfq *scheduler, const void *channel);

/* Process lifecycle hooks; used by proc.c, not direct application APIs. */
void mlfq_on_runnable(struct mlfq *scheduler, struct proc *proc);
void mlfq_on_exit(struct mlfq *scheduler, struct proc *proc);

/* RISC-V cooperative execution backend (not linked in host policy tests).
 * run executes callable processes on their own kernel stacks until idle.
 * Suspended sleepers survive an idle return and can resume in a later run.
 * User-image execution returns PROC_ENOSYS, preserving that process in its
 * queue until a user-mode architecture backend is available.
 * yield/sleep/checkpoint may be called ONLY by the current callable entry.
 * A killed suspended continuation is abandoned and never resumes.
 */
int mlfq_run(struct mlfq *scheduler);
int mlfq_yield(struct mlfq *scheduler);
int mlfq_sleep(struct mlfq *scheduler, const void *channel);
int mlfq_checkpoint(struct mlfq *scheduler);
#endif
