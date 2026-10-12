CROSS ?= riscv64-linux-gnu-
CC := $(CROSS)gcc
QEMU ?= qemu-system-riscv64
KERNEL := build/buddy.elf
SHELL_KERNEL := build/shell.elf
INCLUDE_DIR := src/include
CPPFLAGS += -I$(INCLUDE_DIR)

CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -ffreestanding -fno-builtin -nostdinc \
	-fno-stack-protector -fno-pic -msmall-data-limit=0 -mcmodel=medany \
	-march=rv64imac -mabi=lp64
LDFLAGS := -nostdlib -static -no-pie -Wl,-T,src/qemu_virt.ld \
	-Wl,--build-id=none -Wl,--no-relax

.PHONY: all shell qemu qemu-shell test test-shell stage-test clean
all: $(KERNEL) $(SHELL_KERNEL)
shell: $(SHELL_KERNEL)

PROC_SOURCES := src/proc/proc.c src/proc/proc_fs.c src/proc/syscall.c
SCHED_SOURCES := src/schd/mlfq.c
RISCV_SCHED_SOURCES := src/schd/riscv.c src/asm/swtch.s
MM_SOURCES := src/mm/buddy_mm.c src/mm/kalloc.c
FS_SOURCES := src/fs/inode.c src/fs/fs.c src/utils/mkfs.c
LIST_SOURCE := src/types/list.c
KERNEL_SOURCES := src/asm/boot.S $(MM_SOURCES) $(LIST_SOURCE) src/types/string.c \
	$(PROC_SOURCES) $(SCHED_SOURCES) $(RISCV_SCHED_SOURCES) $(FS_SOURCES)
SHELL_SOURCES := src/utils/shell/qemu_shell.c src/utils/shell/console.c src/utils/shell/ks.c \
	src/utils/shell/shell_fs.c src/utils/shell/editor.c
HEADERS := $(wildcard $(INCLUDE_DIR)/*.h)
HOST_CC ?= cc
PYTHON ?= python3
HOST_CFLAGS := -std=c11 -O2 -Wall -Wextra -Werror -pedantic
TESTS := build/list_test build/buddy_test build/kalloc_test build/callable_test build/proc_test build/mlfq_test

KERNEL_TESTS := tests/qemu_buddy.c tests/inode_test.c tests/proc_fs_test.c

$(KERNEL): $(KERNEL_SOURCES) $(KERNEL_TESTS) tests/inode_test.h tests/proc_fs_test.h src/qemu_virt.ld $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(KERNEL_SOURCES) $(KERNEL_TESTS) -o $@

$(SHELL_KERNEL): $(KERNEL_SOURCES) $(SHELL_SOURCES) src/qemu_virt.ld $(HEADERS) Makefile
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(KERNEL_SOURCES) $(SHELL_SOURCES) -o $@

build/list_test: $(LIST_SOURCE) tests/list_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) $(LIST_SOURCE) tests/list_test.c -o $@

build/buddy_test: $(LIST_SOURCE) src/mm/buddy_mm.c tests/buddy_mm_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) $(LIST_SOURCE) src/mm/buddy_mm.c tests/buddy_mm_test.c -o $@

build/kalloc_test: $(LIST_SOURCE) $(MM_SOURCES) tests/kalloc_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) $(LIST_SOURCE) $(MM_SOURCES) tests/kalloc_test.c -o $@

build/callable_test: tests/callable_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) tests/callable_test.c -o $@

build/proc_test: $(LIST_SOURCE) $(MM_SOURCES) $(FS_SOURCES) $(PROC_SOURCES) $(SCHED_SOURCES) tests/proc_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) $(LIST_SOURCE) $(MM_SOURCES) $(FS_SOURCES) $(PROC_SOURCES) $(SCHED_SOURCES) tests/proc_test.c -o $@

build/mlfq_test: $(LIST_SOURCE) $(MM_SOURCES) $(FS_SOURCES) $(PROC_SOURCES) $(SCHED_SOURCES) tests/mlfq_test.c $(HEADERS) Makefile
	@mkdir -p build
	$(HOST_CC) $(CPPFLAGS) $(HOST_CFLAGS) $(LIST_SOURCE) $(MM_SOURCES) $(FS_SOURCES) $(PROC_SOURCES) $(SCHED_SOURCES) tests/mlfq_test.c -o $@

test: $(TESTS)
	./build/list_test
	./build/buddy_test
	./build/kalloc_test
	./build/callable_test
	./build/proc_test
	./build/mlfq_test

qemu: $(KERNEL)
	$(QEMU) -machine virt -m 128M -smp 1 -nographic -bios none \
		-kernel $(KERNEL) -monitor none -serial stdio

qemu-shell: $(SHELL_KERNEL)
	$(QEMU) -machine virt -m 128M -smp 1 -nographic -bios none \
		-kernel $(SHELL_KERNEL) -monitor none -serial stdio

test-shell: $(SHELL_KERNEL)
	$(PYTHON) tests/qemu_shell_test.py --qemu "$(QEMU)" --kernel $(SHELL_KERNEL)

stage-test: test qemu test-shell

clean:
	$(RM) $(KERNEL) $(SHELL_KERNEL) $(TESTS)
