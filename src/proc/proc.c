#include "proc.h"
#include "mlfq.h"

static void bytes_zero(void *dst, size_t size) {
        unsigned char *p = dst;
        for (size_t i = 0; i < size; i++) p[i] = 0;
}

static void bytes_copy(void *dst, const void *src, size_t size) {
        unsigned char *d = dst;
        const unsigned char *s = src;
        for (size_t i = 0; i < size; i++) d[i] = s[i];
}

static size_t bounded_len(const char *s, size_t limit) {
        size_t n = 0;
        if (s == NULL) return limit;
        while (n < limit && s[n] != '\0') n++;
        return n;
}

static int member(const struct proc_table *table, const struct proc *proc) {
        if (table == NULL || table->allocator == NULL || proc == NULL) return 0;
        for (size_t i = 0; i < PROC_MAX; i++)
                if (&table->entries[i] == proc) return proc->pstatus != UNUSED;
        return 0;
}

static int live(const struct proc_table *table, const struct proc *proc) {
        return member(table, proc) && (proc->pstatus == RUNNABLE ||
               proc->pstatus == RUNNING || proc->pstatus == SLEEPING);
}

static void space_free(struct buddy_allocator *allocator, struct proc_space *space) {
        for (size_t i = 0; i < space->count; i++)
                (void)buddy_free(allocator, &space->pages[i].backing);
        bytes_zero(space, sizeof(*space));
}

static int space_add(struct buddy_allocator *allocator, struct proc_space *space,
                     uint64_t va, unsigned int perm) {
        if (space->count == PROC_MAX_PAGES) return PROC_E2BIG;
        struct block block;
        if (buddy_alloc(allocator, 0, &block) != BUDDY_OK) return PROC_ENOMEM;
        bytes_zero((void *)block.addr, BUDDY_PAGE_SIZE);
        space->pages[space->count++] = (struct proc_page){va, block, perm};
        return 0;
}

static const struct proc_page *page_at(const struct proc_space *space, uint64_t va) {
        uint64_t base = va & ~(uint64_t)(BUDDY_PAGE_SIZE - 1);
        for (size_t i = 0; i < space->count; i++)
                if (space->pages[i].va == base) return &space->pages[i];
        return NULL;
}

static int range_check(const struct proc_space *space, uint64_t va,
                       size_t size, unsigned int perm) {
        if (size > UINT64_MAX - va) return PROC_EFAULT;
        while (size != 0) {
                const struct proc_page *page = page_at(space, va);
                if (page == NULL || (page->perm & perm) != perm) return PROC_EFAULT;
                size_t n = BUDDY_PAGE_SIZE - va % BUDDY_PAGE_SIZE;
                if (n > size) n = size;
                va += n;
                size -= n;
        }
        return 0;
}

/* For checked user copies and trusted image loading alike. */
static void space_copy(const struct proc_space *space, uint64_t va,
                       void *buffer, size_t size, int to_user) {
        unsigned char *p = buffer;
        while (size != 0) {
                const struct proc_page *page = page_at(space, va);
                size_t offset = va % BUDDY_PAGE_SIZE;
                size_t n = BUDDY_PAGE_SIZE - offset;
                if (n > size) n = size;
                void *physical = (void *)(page->backing.addr + offset);
                if (to_user) bytes_copy(physical, p, n);
                else bytes_copy(p, physical, n);
                va += n;
                p += n;
                size -= n;
        }
}

int proc_copyin(const struct proc *proc, void *dst, uint64_t src, size_t size) {
        if (proc == NULL || (size != 0 && dst == NULL)) return PROC_EINVAL;
        int err = range_check(&proc->space, src, size, PROC_READ);
        if (err == 0) space_copy(&proc->space, src, dst, size, 0);
        return err;
}

int proc_copyout(struct proc *proc, uint64_t dst, const void *src, size_t size) {
        if (proc == NULL || (size != 0 && src == NULL)) return PROC_EINVAL;
        int err = range_check(&proc->space, dst, size, PROC_WRITE);
        if (err == 0) space_copy(&proc->space, dst, (void *)src, size, 1);
        return err;
}

int proc_copyinstr(const struct proc *proc, char *dst, uint64_t src, size_t size) {
        if (proc == NULL || dst == NULL || size == 0) return PROC_EINVAL;
        for (size_t i = 0; i < size; i++) {
                if (i > UINT64_MAX - src ||
                    proc_copyin(proc, &dst[i], src + i, 1) != 0) return PROC_EFAULT;
                if (dst[i] == '\0') return 0;
        }
        return PROC_E2BIG;
}

int proc_table_init(struct proc_table *table, struct buddy_allocator *allocator,
                    const struct proc_image *images, size_t image_count) {
        if (table == NULL || allocator == NULL || allocator->pages == NULL ||
            (image_count != 0 && images == NULL)) return PROC_EINVAL;
        bytes_zero(table, sizeof(*table));
        table->allocator = allocator;
        table->images = images;
        table->image_count = image_count;
        table->next_pid = 1;
        for (size_t i = 0; i < PROC_MAX; i++) table->entries[i].pstatus = UNUSED;
        return 0;
}

struct proc *proc_find(struct proc_table *table, uint64_t pid) {
        if (table == NULL || pid == 0) return NULL;
        for (size_t i = 0; i < PROC_MAX; i++)
                if (table->entries[i].pstatus != UNUSED && table->entries[i].pid == pid)
                        return &table->entries[i];
        return NULL;
}

static struct proc *proc_alloc(struct proc_table *table) {
        if (table->next_pid == 0 || table->next_pid > INT64_MAX) return NULL;
        for (size_t i = 0; i < PROC_MAX; i++) {
                struct proc *p = &table->entries[i];
                if (p->pstatus != UNUSED) continue;
                struct block stack;
                if (buddy_alloc(table->allocator, 0, &stack) != BUDDY_OK) return NULL;
                bytes_zero((void *)stack.addr, BUDDY_PAGE_SIZE);
                bytes_zero(p, sizeof(*p));
                p->pid = table->next_pid++;
                p->pstatus = USED;
                p->kstack = stack;
                p->ctx.sp = stack.addr + BUDDY_PAGE_SIZE;
                return p;
        }
        return NULL;
}

static void proc_release(struct proc_table *table, struct proc *proc) {
        if (table->scheduler != NULL) mlfq_on_exit(table->scheduler, proc);
        space_free(table->allocator, &proc->space);
        (void)buddy_free(table->allocator, &proc->kstack);
        bytes_zero(proc, sizeof(*proc));
        proc->pstatus = UNUSED;
}

static const struct proc_image *find_image(const struct proc_table *table,
                                          const char *name, size_t len) {
        for (size_t i = 0; i < table->image_count; i++) {
                const struct proc_image *image = &table->images[i];
                if (bounded_len(image->name, PROC_NAME_MAX) != len) continue;
                size_t j = 0;
                while (j < len && name[j] == image->name[j]) j++;
                if (j == len) return image;
        }
        return NULL;
}

static int image_layout(const struct proc_image *image, size_t *code_pages,
                        size_t *data_pages) {
        size_t limit = (PROC_MAX_PAGES - 1) * BUDDY_PAGE_SIZE;
        if (image->code == NULL || image->code_size == 0 ||
            image->code_size > limit || image->data_size > limit ||
            image->bss_size > limit - image->data_size ||
            (image->data_size != 0 && image->data == NULL) ||
            image->entry_offset >= image->code_size || image->entry_offset % 2 != 0)
                return PROC_ENOEXEC;
        *code_pages = (image->code_size + BUDDY_PAGE_SIZE - 1) / BUDDY_PAGE_SIZE;
        *data_pages = (image->data_size + image->bss_size + BUDDY_PAGE_SIZE - 1) /
                      BUDDY_PAGE_SIZE;
        if (*code_pages + *data_pages + 1 > PROC_MAX_PAGES) return PROC_E2BIG;
        return 0;
}

int proc_exec(struct proc_table *table, struct proc *proc, const char *name,
              size_t argc, const char *const argv[]) {
        if (!member(table, proc) || proc->pstatus == ZOMBIE || proc->killed)
                return PROC_EINVAL;
        if (proc->kind != PROC_USER) return PROC_ENOEXEC;
        size_t len = bounded_len(name, PROC_NAME_MAX);
        if (len == 0 || len == PROC_NAME_MAX) return PROC_EINVAL;
        if (argc > PROC_ARG_MAX || (argc != 0 && argv == NULL)) return PROC_E2BIG;
        size_t arg_len[PROC_ARG_MAX];
        for (size_t i = 0; i < argc; i++) {
                arg_len[i] = bounded_len(argv[i], PROC_ARG_BYTES);
                if (arg_len[i] == PROC_ARG_BYTES) return PROC_E2BIG;
        }
        const struct proc_image *image = find_image(table, name, len);
        if (image == NULL) return PROC_ENOEXEC;
        size_t code_pages, data_pages;
        int err = image_layout(image, &code_pages, &data_pages);
        if (err != 0) return err;
        struct proc_space fresh = {0};
        uint64_t data_base = PROC_CODE_BASE + code_pages * BUDDY_PAGE_SIZE;
        for (size_t i = 0; i < code_pages + data_pages; i++) {
                unsigned int perm = i < code_pages ? PROC_READ | PROC_EXEC :
                                                     PROC_READ | PROC_WRITE;
                err = space_add(table->allocator, &fresh,
                                PROC_CODE_BASE + i * BUDDY_PAGE_SIZE, perm);
                if (err != 0) goto fail;
        }
        err = space_add(table->allocator, &fresh, PROC_STACK_BASE, PROC_READ | PROC_WRITE);
        if (err != 0) goto fail;
        space_copy(&fresh, PROC_CODE_BASE, (void *)image->code, image->code_size, 1);
        space_copy(&fresh, data_base, (void *)image->data, image->data_size, 1);
        struct trapframe tf = {0};
        uint64_t sp = PROC_STACK_TOP;
        uint64_t user_argv[PROC_ARG_MAX + 1] = {0};
        for (size_t i = argc; i > 0; i--) {
                sp -= arg_len[i - 1] + 1;
                space_copy(&fresh, sp, (void *)argv[i - 1], arg_len[i - 1] + 1, 1);
                user_argv[i - 1] = sp;
        }
        sp = (sp - (argc + 1) * sizeof(uint64_t)) & ~UINT64_C(15);
        space_copy(&fresh, sp, user_argv, (argc + 1) * sizeof(uint64_t), 1);
        tf.pc = PROC_CODE_BASE + image->entry_offset;
        tf.x[PROC_SP] = sp;
        tf.x[PROC_A0] = argc;
        tf.x[PROC_A1] = sp;
        /* Commit only after every allocation/copy has succeeded. */
        space_free(table->allocator, &proc->space);
        proc->space = fresh;
        proc->tf = tf;
        bytes_zero(proc->name, sizeof(proc->name));
        bytes_copy(proc->name, image->name, len);
        return 0;
fail:
        space_free(table->allocator, &fresh);
        return err;
}

int proc_create(struct proc_table *table, const char *name, struct proc **out) {
        if (table == NULL || table->allocator == NULL || out == NULL) return PROC_EINVAL;
        struct proc *p = proc_alloc(table);
        if (p == NULL) return PROC_ENOMEM;
        p->kind = PROC_USER;
        int err = proc_exec(table, p, name, 0, NULL);
        if (err != 0) { proc_release(table, p); return err; }
        p->pstatus = RUNNABLE;
        if (table->scheduler != NULL) mlfq_on_runnable(table->scheduler, p);
        *out = p;
        return 0;
}

int64_t proc_fork(struct proc_table *table, struct proc *parent) {
        if (!live(table, parent) || parent->killed)
                return PROC_EINVAL;
        if (parent->kind != PROC_USER) return PROC_ENOSYS;
        if (parent->space.count == 0) return PROC_EINVAL;
        struct proc *child = proc_alloc(table);
        if (child == NULL) return PROC_ENOMEM;
        child->kind = PROC_USER;
        for (size_t i = 0; i < parent->space.count; i++) {
                const struct proc_page *page = &parent->space.pages[i];
                int err = space_add(table->allocator, &child->space, page->va, page->perm);
                if (err != 0) { proc_release(table, child); return err; }
                bytes_copy((void *)child->space.pages[i].backing.addr,
                           (void *)page->backing.addr, BUDDY_PAGE_SIZE);
        }
        child->tf = parent->tf;
        child->tf.x[0] = 0;
        child->tf.x[PROC_A0] = 0;
        child->parent = parent;
        bytes_copy(child->name, parent->name, sizeof(child->name));
        /* ra remains zero until the architecture supplies a user-return entry. */
        child->pstatus = RUNNABLE;
        if (table->scheduler != NULL) mlfq_on_runnable(table->scheduler, child);
        parent->tf.x[PROC_A0] = child->pid;
        return (int64_t)child->pid;
}

int proc_kill(struct proc_table *table, uint64_t pid) {
        struct proc *p = proc_find(table, pid);
        if (p == NULL || !live(table, p)) return PROC_ESRCH;
        p->killed = 1;
        if (p->pstatus == SLEEPING) p->pstatus = RUNNABLE;
        if (table->scheduler != NULL && p->pstatus == RUNNABLE)
                mlfq_on_runnable(table->scheduler, p);
        return 0;
}

int proc_exit(struct proc_table *table, struct proc *proc, int status) {
        if (!live(table, proc)) return PROC_EINVAL;
        proc->exit_status = status;
        proc->pstatus = ZOMBIE;
        if (table->scheduler != NULL) mlfq_on_exit(table->scheduler, proc);
        for (size_t i = 0; i < PROC_MAX; i++)
                if (table->entries[i].parent == proc) table->entries[i].parent = NULL;
        return 0;
}

int proc_reap(struct proc_table *table, uint64_t pid) {
        struct proc *p = proc_find(table, pid);
        if (p == NULL) return PROC_ESRCH;
        if (p->pstatus != ZOMBIE || table->current == p || p->entry_active)
                return PROC_EBUSY;
        proc_release(table, p);
        return 0;
}

int proc_activate(struct proc_table *table, struct proc *proc) {
        if (!live(table, proc)) return PROC_EINVAL;
        if (table->current != NULL || proc->pstatus != RUNNABLE) return PROC_EBUSY;
        if (proc->schedule.queued) return PROC_EBUSY;
        if (proc->killed) { (void)proc_exit(table, proc, -1); return PROC_ESRCH; }
        table->current = proc;
        proc->pstatus = RUNNING;
        return 0;
}

int proc_deactivate(struct proc_table *table) {
        if (table == NULL || table->current == NULL) return PROC_EINVAL;
        if (table->scheduler != NULL) return PROC_EBUSY;
        if (table->current->entry_active) return PROC_EBUSY;
        if (table->current->pstatus == RUNNING) table->current->pstatus = RUNNABLE;
        table->current = NULL;
        return 0;
}

int proc_create_callable(struct proc_table *table, const char *name,
                         const struct callable *entry, struct proc **out) {
        if (table == NULL || table->allocator == NULL || out == NULL ||
            !callable_valid(entry)) return PROC_EINVAL;
        size_t len = bounded_len(name, PROC_NAME_MAX);
        if (len == 0 || len == PROC_NAME_MAX) return PROC_EINVAL;
        /* Copy borrowed descriptors/name before reusing a table slot. */
        struct callable saved_entry = *entry;
        char saved_name[PROC_NAME_MAX] = {0};
        bytes_copy(saved_name, name, len);
        struct proc *p = proc_alloc(table);
        if (p == NULL) return PROC_ENOMEM;
        p->kind = PROC_KERNEL;
        p->entry = saved_entry;
        bytes_copy(p->name, saved_name, sizeof(p->name));
        p->pstatus = RUNNABLE;
        if (table->scheduler != NULL) mlfq_on_runnable(table->scheduler, p);
        *out = p;
        return 0;
}

int proc_run_callable(struct proc_table *table) {
        if (table == NULL || !member(table, table->current)) return PROC_EINVAL;
        struct proc *p = table->current;
        if (p->kind != PROC_KERNEL) return PROC_EINVAL;
        if (p->entry_started || p->entry_active) return PROC_EBUSY;
        if (p->pstatus != RUNNING) return PROC_EINVAL;
        if (p->killed) return proc_exit(table, p, -1);
        p->entry_started = 1;
        p->entry_active = 1;
        enum status invoked = callable_invoke(&p->entry, &p->result);
        p->entry_active = 0;
        int status = invoked == SUCCESS ? 0 : PROC_EINVAL;
        if (p->result.tag == fi32v || p->result.tag == fi32p)
                status = p->result.value.i32;
        if (p->killed) status = -1;
        /* An entry may explicitly set its exit code through proc_exit. */
        if (p->pstatus != ZOMBIE) (void)proc_exit(table, p, status);
        else if (p->killed) p->exit_status = -1;
        return invoked == SUCCESS ? 0 : PROC_EINVAL;
}
