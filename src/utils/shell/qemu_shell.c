#include "shell.h"
#include "console.h"
#include "qemu_virt.h"
#include "kalloc.h"

#define SHELL_PAGES 2048u /* Eight MiB, outside kernel/BSS/boot-stack storage. */

extern char __kernel_end[];
static struct buddy_allocator allocator;
static struct buddy_page metadata[SHELL_PAGES];
static struct proc_table processes;
static struct mlfq scheduler;
static struct fs filesystem;

/* Flat images for lifecycle inspection, not yet executable in user mode. */
static const unsigned char init_code[] = {0x73, 0, 0, 0};
static const unsigned char alt_code[] = {1, 0, 0x73, 0, 0, 0};
static const unsigned char init_data[] = "init data";
static const unsigned char alt_data[] = "alternate data";
static const struct proc_image images[] = {
        {"init", init_code, sizeof(init_code), init_data, sizeof(init_data), 64, 0},
        {"alt", alt_code, sizeof(alt_code), alt_data, sizeof(alt_data), 128, 2},
};

void kmain(void) {
        uptr_t base = BUDDY_ALIGN_UP(__kernel_end);
        struct buddy_arena arena = {base, SHELL_PAGES * BUDDY_PAGE_SIZE};
        if (base < QEMU_RAM_BASE || base > QEMU_RAM_END ||
            arena.size > QEMU_RAM_END - base ||
            buddy_init(&allocator, &arena, metadata, SHELL_PAGES) != BUDDY_OK ||
            kalloc_init(&allocator) != KALLOC_OK ||
            make_filesystem(&filesystem) != FS_OK) {
                console_puts("SHELL INIT FAIL\n");
                console_shutdown(1);
        }
        struct inode *root;
        if (inode_get(&filesystem, 1, &root) != FS_OK ||
            proc_image_install(root, "init", &images[0]) != 0 ||
            proc_image_install(root, "alt", &images[1]) != 0 ||
            inode_put(root) != FS_OK ||
            proc_table_init_fs(&processes, &allocator, &filesystem) != 0 ||
            mlfq_init(&scheduler, &processes, NULL) != 0) {
                console_puts("SHELL INIT FAIL\n");
                console_shutdown(1);
        }
        shell_main_loop(&allocator, &processes, &scheduler);
}
