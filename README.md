# os_learn

An experimental RISC-V kernel with buddy and slab memory management and a basic process
lifecycle module and four-level MLFQ scheduling. Hardware user address spaces,
user-mode trap entry and interrupt-driven preemption are still pending.

The kernel uses project-owned C types and memory routines, and builds with
`-nostdinc`, `-ffreestanding`, `-fno-builtin`, and `-nostdlib`. Kernel sources
and new kernel tests must not depend on standard C library headers or runtime
functions. Historical host tests use libc only for their test harness.

Processes load flat executable files from the RAM filesystem and support
callable kernel entries. Each process owns a current-directory reference and
16 file-descriptor slots. Fork and dup share open-file offsets; exec preserves
descriptors and cwd; exit releases them before reap frees image/stack pages.
The callback
API lives in `src/include/callable.h`; `types.h` continues to expose it for
existing code. Create a kernel process with `proc_create_callable`, activate it,
and invoke `proc_run_callable` explicitly for a synchronous call on the caller's
stack. Alternatively, attach MLFQ and use `mlfq_run` to execute callbacks on
their own kernel stacks, with cooperative yield, sleep and wakeup. Both paths
capture typed results and leave zombies for explicit reaping.

## Project layout

```text
src/
  include/          All project headers
  asm/              Boot assembly and RISC-V context switching
  mm/               Buddy pages, slab-backed kalloc and design notes
  types/            Lists and freestanding string functions
  fs/               RAM inodes, directory namespace, file I/O and design notes
  proc/             Process implementation, syscall dispatch and design notes
  schd/             MLFQ policy, RISC-V execution backend and design notes
  utils/shell/      UART console, kernel stage-test shell and boot entry
  qemu_virt.ld      RISC-V linker script
tests/
  list_test.c       Host list tests
  buddy_mm_test.c   Host buddy data-structure tests
  kalloc_test.c     Host slab and kernel heap tests
  callable_test.c   Host callable tests
  proc_test.c       Host allocator and process lifecycle tests
  mlfq_test.c       Host MLFQ policy and lifecycle tests
  qemu_buddy.c      Freestanding QEMU test entry point (kmain)
  inode_test.c      Freestanding inode tests run by the QEMU entry point
  proc_fs_test.c    Freestanding exec, fd/cwd, file-syscall and rollback tests
  qemu_shell_test.py Automated shell checks through QEMU's serial port
  mm_test.c         Legacy page-allocator experiment
  stdfd_test.c      Host standard-file-descriptor experiment
build/              Generated executables, kernel images and shell test log
```

## Header search path

The Makefile configures the project include directory once, using the standard
preprocessor flags variable shared by host and RISC-V builds:

```make
INCLUDE_DIR := src/include
CPPFLAGS += -I$(INCLUDE_DIR)
```

Keep project headers in `src/include/` and include them by name:

```c
#include "proc.h"
#include "buddy_mm.h"
#include "ktypes.h"
```

Use project headers for kernel code: `ktypes.h` provides `u64_t`, `ksize_t`,
`uptr_t` and related primitive types; `kstring.h` declares the kernel's own
memory routines. `-nostdinc` prevents accidental system-header dependencies.
Compiler warning/optimization flags stay in `CFLAGS` or
`HOST_CFLAGS`; linker flags stay in `LDFLAGS`.

When compiling manually from the repository root, also pass `-Isrc/include`.
For an editor configured separately, use `${workspaceFolder}/src/include` as
its project include directory and select the appropriate host or RISC-V compiler.

### VS Code with clangd

Generate the compilation database with Bear from the repository root:

```sh
bear -- make -B all test
```

This creates `compile_commands.json` in the project root, which clangd discovers
automatically. It records the actual include paths and flags for both the
RISC-V kernel and supported host tests. `-B` forces compilation even when build
outputs are already current, so Bear can capture the commands.

Regenerate after changing build flags or adding sources to the build, then run
**clangd: Restart language server** in VS Code if diagnostics have not refreshed.
Sources outside these build targets are not captured by Bear.

## Build and test

```sh
make test       # Six supported host test suites; requires a C compiler
make            # Build the RISC-V smoke-test kernel and interactive shell
make qemu       # Check BUDDY, KALLOC, INODE, PROC FS, PROC, CALLABLE PROC and MLFQ OK
make qemu-shell # Boot the interactive shell; type help at the os> prompt
make test-shell # Automated UART shell checks; also requires Python 3
make stage-test # Host tests, QEMU smoke test and UART shell checks
```

For hands-on filesystem testing, boot with `make qemu-shell` and use `pwd`,
`cd`, `mkdir`, `touch`, `ls`, `cat`, `rm`, and `editor FILE`. The shell starts
at `/` and accepts quoted paths. Its line editor supports append/insert,
replacement, deletion, numbered output, save and discard, with a 16 KiB text
limit. See the [interactive walkthrough](src/utils/shell/shell.md). Files live
in RAM and disappear at shutdown. `make stage-test` includes 384 UART checks.

The RISC-V build requires `riscv64-linux-gnu-gcc`; the boot check requires
`qemu-system-riscv64`. Override `CROSS`, `HOST_CC`, or `QEMU` as needed.
Artifacts remain under `build/`. Changing headers or the Makefile rebuilds the
affected targets.

`mm_test.c` and `stdfd_test.c` are retained experiments outside `make test`.
The old page allocator needs kernel linker symbols and must not manage the same
RAM as the buddy allocator. QEMU checks the C process/syscall logic and executes
callable kernel processes with MLFQ and context switching; it does not execute
user processes yet. MLFQ's defaults are quanta of 1/2/4/8 ticks, CPU allotments
of 2/4/8/16 ticks, and a priority boost every 64 ticks. No hardware timer handler
is installed; tests feed ticks explicitly through `mlfq_tick`.

Kernel heap allocations use `kalloc_init(&allocator)`, `kalloc(size)`, and
`kfree(ptr)` from `kalloc.h`. Small objects share slab pages; larger requests
use buddy blocks. See [kalloc design and usage](src/mm/kalloc.md).

The RAM inode layer provides reference/link ownership, directories and path
lookup, sparse file I/O, truncation and independent open-file cursors. See
[inode design and usage](src/fs/fs.md) and the
[project map and implementation plan](docs/project-map.md). Process file APIs
in `proc_fs.h` and file syscalls now use this layer. Initialize new process
tables with `proc_table_init_fs`; the original registry initializer remains
for compatibility tests. The shell installs `/init` and `/alt` as executable
inodes at boot. The small versioned executable format is documented in
[the process guide](src/proc/proc.md). Disk persistence and hardware user-mode
execution remain future work.

See [buddy design](src/mm/buddy_mm.md), [process design](src/proc/proc.md), and
[MLFQ design and usage](src/schd/mlfq.md). For an interactive stage test, see
the [shell guide](src/utils/shell/shell.md).
