#include "mlfq.h"

const struct mlfq_config mlfq_default_config = {
        .quantum = {1, 2, 4, 8},
        .allotment = {2, 4, 8, 16},
        .boost_interval = 64,
};

static int valid(const struct mlfq *s) {
        return s != NULL && s->table != NULL && s->table->scheduler == s;
}

static void reset_budget(struct mlfq *s, struct proc *p) {
        p->schedule.slice_left = s->config.quantum[p->schedule.level];
        p->schedule.budget_left = s->config.allotment[p->schedule.level];
}

static void remove_ready(struct mlfq *s, struct proc *p) {
        if (!p->schedule.queued) return;
        list_remove(&s->queues[p->schedule.level], &p->schedule.ready_node);
        p->schedule.queued = 0;
}

void mlfq_on_runnable(struct mlfq *s, struct proc *p) {
        if (!valid(s) || p == NULL) return;
        if (p->schedule.owner == NULL) {
                p->schedule.owner = s;
                node_init(&p->schedule.ready_node, p);
                p->schedule.level = 0;
                reset_budget(s, p);
        }
        if (p->schedule.owner != s || p->pstatus != RUNNABLE || p->schedule.queued)
                return;
        p->schedule.wait_channel = NULL;
        list_push_back(&s->queues[p->schedule.level], &p->schedule.ready_node);
        p->schedule.queued = 1;
}

void mlfq_on_exit(struct mlfq *s, struct proc *p) {
        if (!valid(s) || p == NULL || p->schedule.owner != s) return;
        remove_ready(s, p);
        p->schedule.wait_channel = NULL;
        /* The continuation is no longer on a CPU. Discarding it is safe;
         * borrowed callable context/results still belong to their caller.
         */
        if (p != s->table->current && p->schedule.suspended) {
                p->entry_active = 0;
                p->schedule.suspended = 0;
        }
}

int mlfq_init(struct mlfq *s, struct proc_table *table,
              const struct mlfq_config *config) {
        if (s == NULL || table == NULL || table->allocator == NULL)
                return PROC_EINVAL;
        if (table->current != NULL || table->scheduler != NULL) return PROC_EBUSY;
        struct mlfq_config chosen = config == NULL ? mlfq_default_config : *config;
        for (size_t i = 0; i < MLFQ_LEVELS; i++)
                if (chosen.quantum[i] == 0 || chosen.allotment[i] < chosen.quantum[i])
                        return PROC_EINVAL;
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &table->entries[i];
                if (p->pstatus == RUNNING || p->pstatus == USED || p->entry_active)
                        return PROC_EBUSY;
        }
        *s = (struct mlfq){ .table = table, .config = chosen };
        for (size_t i = 0; i < MLFQ_LEVELS; i++) list_init(&s->queues[i]);
        table->scheduler = s;
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &table->entries[i];
                if (p->pstatus == RUNNABLE || p->pstatus == SLEEPING) {
                        enum state original = p->pstatus;
                        /* Admit sleepers without putting them in a ready queue. */
                        p->pstatus = RUNNABLE;
                        mlfq_on_runnable(s, p);
                        if (original == SLEEPING) remove_ready(s, p);
                        p->pstatus = original;
                }
        }
        return 0;
}

int mlfq_destroy(struct mlfq *s) {
        if (!valid(s)) return PROC_EINVAL;
        if (s->driver_active || s->table->current != NULL) return PROC_EBUSY;
        for (size_t i = 0; i < PROC_MAX; i++)
                if (s->table->entries[i].entry_active ||
                    (s->table->entries[i].schedule.owner == s &&
                     s->table->entries[i].pstatus == SLEEPING)) return PROC_EBUSY;
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &s->table->entries[i];
                if (p->schedule.owner != s) continue;
                remove_ready(s, p);
                p->schedule = (struct proc_schedule){0};
        }
        s->table->scheduler = NULL;
        s->table = NULL;
        return 0;
}

int mlfq_dispatch(struct mlfq *s, struct proc **out) {
        if (!valid(s) || out == NULL) return PROC_EINVAL;
        if (s->table->current != NULL || s->in_process) return PROC_EBUSY;
        for (size_t level = 0; level < MLFQ_LEVELS; level++) {
                struct node *node;
                while ((node = list_pop_front(&s->queues[level])) != NULL) {
                        struct proc *p = node->data;
                        p->schedule.queued = 0;
                        if (p->pstatus != RUNNABLE) continue;
                        if (p->killed) {
                                (void)proc_exit(s->table, p, -1);
                                continue;
                        }
                        int err = proc_activate(s->table, p);
                        if (err != 0) {
                                mlfq_on_runnable(s, p);
                                return err;
                        }
                        p->schedule.suspended = 0;
                        s->reschedule = 0;
                        *out = p;
                        return 1;
                }
        }
        return 0;
}

static void account_stop(struct mlfq *s, struct proc *p) {
        if (p->schedule.budget_left == 0) {
                if (p->schedule.level + 1 < MLFQ_LEVELS) p->schedule.level++;
                reset_budget(s, p);
        } else if (p->schedule.slice_left == 0) {
                p->schedule.slice_left = s->config.quantum[p->schedule.level];
        }
}

int mlfq_complete(struct mlfq *s, enum mlfq_stop reason, const void *channel) {
        if (!valid(s)) return PROC_EINVAL;
        struct proc *p = s->table->current;
        if (p == NULL || p->schedule.owner != s) return PROC_EINVAL;
        if (s->in_process || (p->entry_active && !p->schedule.suspended))
                return PROC_EBUSY;
        if (reason < MLFQ_YIELD || reason > MLFQ_EXIT ||
            (reason == MLFQ_BLOCK && channel == NULL)) return PROC_EINVAL;
        if (reason == MLFQ_EXIT && p->pstatus != ZOMBIE && !p->killed)
                return PROC_EINVAL;
        if (p->pstatus != RUNNING && p->pstatus != ZOMBIE) return PROC_EINVAL;
        if (p->killed && p->pstatus != ZOMBIE) (void)proc_exit(s->table, p, -1);
        /* Architecture has switched away. This is the scheduler-only release
         * path; proc_deactivate deliberately rejects an attached scheduler.
         */
        s->table->current = NULL;
        s->reschedule = 0;
        if (p->pstatus == ZOMBIE) {
                mlfq_on_exit(s, p);
                return 0;
        }
        account_stop(s, p);
        if (reason == MLFQ_BLOCK) {
                p->pstatus = SLEEPING;
                p->schedule.wait_channel = channel;
        } else {
                p->pstatus = RUNNABLE;
                mlfq_on_runnable(s, p);
        }
        return 0;
}

int mlfq_should_reschedule(const struct mlfq *s) {
        if (!valid(s)) return PROC_EINVAL;
        const struct proc *p = s->table->current;
        if (p == NULL) return 0;
        if (p->schedule.owner != s) return PROC_EINVAL;
        if (p->killed || p->pstatus == ZOMBIE || s->reschedule ||
            p->schedule.slice_left == 0 || p->schedule.budget_left == 0) return 1;
        for (size_t i = 0; i < p->schedule.level; i++)
                if (!list_empty(&s->queues[i])) return 1;
        return 0;
}

int mlfq_boost(struct mlfq *s) {
        if (!valid(s)) return PROC_EINVAL;
        /* Drain in priority/FIFO order before resetting levels, so each
         * embedded node stays linked to exactly one queue.
         */
        struct list_meta gathered;
        list_init(&gathered);
        for (size_t i = 0; i < MLFQ_LEVELS; i++) {
                struct node *node;
                while ((node = list_pop_front(&s->queues[i])) != NULL) {
                        struct proc *p = node->data;
                        p->schedule.queued = 0;
                        list_push_back(&gathered, node);
                }
        }
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &s->table->entries[i];
                if (p->schedule.owner != s || p->pstatus == ZOMBIE) continue;
                p->schedule.level = 0;
                reset_budget(s, p);
        }
        struct node *node;
        while ((node = list_pop_front(&gathered)) != NULL) {
                struct proc *p = node->data;
                mlfq_on_runnable(s, p);
        }
        s->boost_elapsed = 0;
        if (s->table->current != NULL) s->reschedule = 1;
        return 0;
}

int mlfq_tick(struct mlfq *s) {
        if (!valid(s)) return PROC_EINVAL;
        struct proc *p = s->table->current;
        if (p != NULL && p->schedule.owner != s) return PROC_EINVAL;
        if (s->ticks != UINT64_MAX) s->ticks++;
        if (p != NULL && p->pstatus == RUNNING) {
                if (p->schedule.slice_left != 0) p->schedule.slice_left--;
                if (p->schedule.budget_left != 0) p->schedule.budget_left--;
        }
        if (s->config.boost_interval != 0) {
                s->boost_elapsed++;
                if (s->boost_elapsed == s->config.boost_interval)
                        (void)mlfq_boost(s);
        }
        return mlfq_should_reschedule(s);
}

int mlfq_wakeup(struct mlfq *s, const void *channel) {
        if (!valid(s) || channel == NULL) return PROC_EINVAL;
        int count = 0;
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &s->table->entries[i];
                if (p->schedule.owner != s || p->pstatus != SLEEPING ||
                    p->schedule.wait_channel != channel) continue;
                p->pstatus = RUNNABLE;
                mlfq_on_runnable(s, p);
                count++;
        }
        return count;
}
