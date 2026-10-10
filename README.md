# os_learn

An experimental RISC-V kernel with buddy memory management and a basic process
lifecycle module and four-level MLFQ scheduling. Hardware user address spaces,
user-mode trap entry and interrupt-driven preemption are still pending.

Processes support flat user images and callable kernel entries. The callback
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
  proc/             Process implementation, syscall dispatch and design notes
  schd/             MLFQ policy, RISC-V execution backend and design notes
  console.c         Polled UART console and basic line editing
  ks.c              Interactive kernel stage-test shell
  qemu_shell.c      Shell boot entry and built-in test images
  *.c               Other kernel implementations
  qemu_virt.ld      RISC-V linker script
tests/
  list_test.c       Host list tests
  buddy_mm_test.c   Host buddy data-structure tests
  callable_test.c   Host callable tests
  proc_test.c       Host allocator and process lifecycle tests
  mlfq_test.c       Host MLFQ policy and lifecycle tests
  qemu_buddy.c      Freestanding QEMU test entry point (kmain)
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
#include <stdint.h>
```

Use quotes for project headers and angle brackets for standard library headers.
`-I` adds this project's directory without replacing the compiler's standard
header search paths. Compiler warning/optimization flags stay in `CFLAGS` or
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
make test       # Five supported host test suites; requires a C compiler
make            # Build the RISC-V smoke-test kernel and interactive shell
make qemu       # Check for BUDDY OK, PROC OK, CALLABLE PROC OK and MLFQ OK
make qemu-shell # Boot the interactive shell; type help at the os> prompt
make test-shell # Automated UART shell checks; also requires Python 3
make stage-test # Host tests, QEMU smoke test and UART shell checks
```

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

See [buddy design](src/buddy_mm.md), [process design](src/proc/proc.md), and
[MLFQ design and usage](src/schd/mlfq.md). For an interactive stage test, see
the [shell guide](src/shell.md).
