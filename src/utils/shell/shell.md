# QEMU stage-test shell

Build and boot from the repository root:

```sh
make qemu-shell
```

The `os>` prompt runs on the kernel boot stack. Commands call the existing
buddy, process and MLFQ APIs. Callable jobs run on their own kernel stacks;
the console uses QEMU virt's polled UART, so neither a filesystem nor UART
interrupts are required. Type `help` to list commands and `quit` to shut down
QEMU cleanly. Backspace/DEL edits the line, Ctrl-U clears it, Ctrl-C cancels
it, and Ctrl-D on an empty line shuts down.

## Scheduling walkthrough

Enter each command on a separate line:

```text
mem
demo
ps
queues
wake all
run
ps
reap all
mem
quit
```

`demo` requires an empty process table. It creates three CPU jobs with six
steps each and one sleeping job, then runs the scheduler. CPU jobs print their
PID, step, priority level and remaining CPU budget. Their output shows MLFQ
interleaving and demotion. They finish as zombies; the fourth job remains
sleeping. `wake all` makes it runnable, and `run` resumes its saved stack.
`reap all` releases the four jobs' resources. The final `mem` should show
2048 free pages and one order-11 block again.

Jobs advance the scheduler clock once per step, then yield or check whether
rescheduling is required. Time is software-fed; hardware timer preemption is
still pending. The shell waits while `run` executes jobs and returns when
there are no runnable jobs. Creating or waking a job does not run it until
you enter `run`.

## Basic operations

| Commands | What to inspect |
| --- | --- |
| `ps`, `show PID` | State, exit status, saved registers, stack and image mappings |
| `queues`, `boost`, `tick [N]` | Ready queues, priority budgets and test clock |
| `spawn cpu [N]` | Work that checks for rescheduling after each step |
| `spawn yield [N]` | Work that voluntarily yields after every step |
| `spawn sleep [N]`, `wake PID\|all` | A blocked continuation and its later resumption |
| `kill PID\|all`, `run`, `reap PID\|all` | Deferred termination and resource reclamation |
| `exit PID [CODE]`, `reap PID` | Explicit exit of an inactive process |
| `mem`, `alloc ORDER`, `free ID\|all` | Buddy allocation, exhaustion, release and coalescing |
| `image init\|alt`, `fork PID`, `exec PID init\|alt [args...]` | Saved user-image creation, deep copying and replacement |
| `echo [words...]` | Basic command parsing and console output |

Use the PID or block ID printed by the creation command. IDs continue
increasing after objects are reclaimed. Jobs default to six steps; accepted
counts are 1–1024. Allocation orders are 0–11, with sizes of
`4096 * 2^ORDER` bytes. The shell manages an 8 MiB buddy arena after the kernel,
BSS and boot stack; this is a subset of QEMU's 128 MiB RAM. Up to 16 manual
blocks and 32 processes can be tracked. Zombies retain resources until reaped.

For example, create a sleeping job with `spawn sleep 2`, then enter `run`.
Use its printed PID with `show PID` and `kill PID`. Another `run` processes
the pending termination without resuming the killed callback. `show PID`
then reports a zombie with exit status -1; `reap PID` frees its stack.

For user-image lifecycle testing, use `image init`, `fork PID`, and
`exec CHILD_PID alt hello world`. Inspect both processes with `show`:
fork copies their backing pages, and exec installs the alternate image with
two arguments. These images are saved software mappings: user-mode execution,
hardware address-space switching and trap handling are not implemented yet.
`run` reports an unsupported-operation error when it selects a live user
image. Enter `kill all`, `run`, and `reap all` to clear these test images
before continuing callable scheduling tests.

The parser accepts whitespace-separated words, up to 255 characters and
12 tokens per line. `exec` accepts up to eight argument words. Quoting,
command chaining, pipes, redirection and filesystem commands are not provided.

## Automated verification

```sh
make stage-test
```

This runs the five host suites, the existing QEMU kernel smoke test, and
`tests/qemu_shell_test.py`. The Python harness drives a real QEMU UART and
checks job execution, sleep/wakeup, kill/reap, fork/exec mappings, exhaustion,
malformed input, line editing and memory recovery. It prints `SHELL OK` and
saves the serial transcript to `build/shell_test.log`. Run `make test-shell`
to execute just the shell checks.

`make` builds both `build/buddy.elf` and `build/shell.elf`. Regenerate clangd's
compilation database after source or build changes with:

```sh
bear -- make -B all test
```
