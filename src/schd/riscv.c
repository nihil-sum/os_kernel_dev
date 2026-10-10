#include "mlfq.h"

_Static_assert(sizeof(struct context) == 14 * sizeof(uint64_t), "swtch context size");
_Static_assert(offsetof(struct context, ra) == 0, "swtch ra offset");
_Static_assert(offsetof(struct context, sp) == 8, "swtch sp offset");
_Static_assert(offsetof(struct context, s11) == 104, "swtch s11 offset");

/* The current backend is single-hart, with no nested scheduler loops. */
static struct mlfq *active;

static void entry_trampoline(void) {
        struct mlfq *s = active;
        (void)proc_run_callable(s->table);
        s->pending = MLFQ_EXIT;
        s->pending_channel = NULL;
        swtch(&s->table->current->ctx, &s->dispatcher);
        /* A finished entry must never be selected again. */
        for (;;) __asm__ volatile("wfi");
}

int mlfq_run(struct mlfq *s) {
        if (s == NULL || s->table == NULL || s->table->scheduler != s)
                return PROC_EINVAL;
        if (active != NULL || s->driver_active || s->table->current != NULL)
                return PROC_EBUSY;
        active = s;
        s->driver_active = 1;
        int result = 0;
        for (;;) {
                struct proc *p;
                int picked = mlfq_dispatch(s, &p);
                if (picked <= 0) { result = picked; break; }
                if (p->kind != PROC_KERNEL) {
                        (void)mlfq_complete(s, MLFQ_YIELD, NULL);
                        result = PROC_ENOSYS;
                        break;
                }
                if (!p->schedule.context_ready) {
                        p->ctx.ra = (uintptr_t)entry_trampoline;
                        p->schedule.context_ready = 1;
                }
                s->pending = MLFQ_YIELD;
                s->pending_channel = NULL;
                s->in_process = 1;
                swtch(&s->dispatcher, &p->ctx);
                s->in_process = 0;
                p->schedule.suspended = p->entry_active;
                result = mlfq_complete(s, s->pending, s->pending_channel);
                if (result != 0) break;
        }
        s->driver_active = 0;
        active = NULL;
        return result;
}

static int suspend(struct mlfq *s, enum mlfq_stop reason, const void *channel) {
        if (s == NULL || active != s || !s->in_process || s->table->current == NULL ||
            s->table->current->kind != PROC_KERNEL ||
            s->table->current->pstatus != RUNNING || !s->table->current->entry_active)
                return PROC_EINVAL;
        s->pending = reason;
        s->pending_channel = channel;
        swtch(&s->table->current->ctx, &s->dispatcher);
        return 0;
}

int mlfq_yield(struct mlfq *s) {
        return suspend(s, MLFQ_YIELD, NULL);
}

int mlfq_sleep(struct mlfq *s, const void *channel) {
        if (channel == NULL) return PROC_EINVAL;
        return suspend(s, MLFQ_BLOCK, channel);
}

int mlfq_checkpoint(struct mlfq *s) {
        if (s == NULL || active != s || !s->in_process) return PROC_EINVAL;
        int requested = mlfq_should_reschedule(s);
        if (requested <= 0) return requested;
        return suspend(s, MLFQ_PREEMPT, NULL);
}
