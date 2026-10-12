# QEMU stage-test shell

Build and boot from the repository root:

```sh
make qemu-shell
```

The `os>` prompt runs on the kernel boot stack. Commands call the existing
buddy, slab, inode, process and MLFQ APIs. Startup creates a RAM filesystem,
installs `/init` and `/alt` executable files, and initializes the process table
with `proc_table_init_fs`. Callable jobs run on their own kernel stacks.
The shell retains its own cwd, initially `/`, without consuming a process slot.
Ordinary filesystem commands use this cwd; PID-based test commands continue to
use the selected process's cwd.
The console uses QEMU virt's polled UART; UART interrupts are not installed.
Type `help` to list commands and `quit` to shut down
QEMU cleanly. Backspace/DEL edits the line, Ctrl-U clears it, Ctrl-C cancels
it, and Ctrl-D on an empty line shuts down.

## Interactive filesystem walkthrough

Boot with `make qemu-shell`, then enter:

```text
pwd
ls
mkdir work
cd work
touch notes
editor notes
```

The editor prints its commands and shows `edit>`. Enter the following sequence;
`a` switches to `text>` for the two text lines, and `.` ends text input:

```text
a
Hello from my kernel!
This file lives in the RAM filesystem.
.
p
wq
```

`p` displays numbered lines; `wq` saves and returns to `os>`. Inspect and
clean up with:

```text
cat notes
ls
cd ..
pwd
rm work/notes
rm work
```

`cd [PATH]` defaults to `/`; `pwd` prints the canonical absolute path.
`mkdir PATH` creates one directory, requiring its parent to exist. `touch PATH`
creates an empty regular file or preserves an existing regular file's bytes;
the filesystem has no timestamps. `ls [PATH]` defaults to cwd and prints type,
size, inode number and name. A directory's size is its entry count; a regular
file's size is bytes. Directory names end with `/`; synthetic `.` and `..`
are omitted. `ls FILE` prints that file's metadata. `cat PATH` displays a file
and renders non-text control bytes as `?`. `rm PATH` removes a file or empty
directory, protecting root, `.`/`..`, and the shell's current directory.
Absolute/relative paths, repeated slashes, and `.`/`..` use existing inode lookup.

Commands support single/double quotes and backslash escapes, for example
`touch "my notes"`, `editor 'my notes'`, or `rm my\ notes`. Inside single quotes,
backslashes are literal. Unterminated quotes and escapes reject the command.

## Line editor

`editor PATH` (alias `edit PATH`) opens an existing regular text file or starts
a new buffer. A new file is published only when saved. Edits use a heap buffer
allocated through `kalloc`, with a 16 KiB file limit. Files must contain ASCII
text with LF newlines; tabs and an unterminated final line are supported.
Binary files and oversized files are refused without changing them.

| Editor command | Action |
| --- | --- |
| `p` | Print numbered lines |
| `a` | Append text lines until a single `.` |
| `i N` | Insert text before line N; last+1 appends |
| `r N` | Replace line N with the next input line |
| `d N` | Delete line N |
| `w`, `wq` | Save; save and quit |
| `q`, `q!` | Quit when saved; discard unsaved changes |
| `help` | Show editor commands |

Line numbers begin at 1. In append/insert mode, `..` enters a literal dot line;
empty lines, spaces and tabs are retained. Replacement mode accepts any single
line, including `.`. New or replaced lines end in LF. Input lines are limited
to 255 characters; an overlong line or a change exceeding 16 KiB is rejected
without truncating the previous buffer. Ctrl-C leaves the current input mode,
retaining lines already staged; Ctrl-D ends text input or requests `q` at the
editor prompt. Unsaved changes require `w` or `q!` before quitting. Ctrl-D
inside the editor does not shut down QEMU.

Saving an existing file writes once through atomic inode I/O, then truncates
the old tail after success. Its inode identity and open process descriptors
remain valid. Saving a new file fills an unnamed inode before linking it.
Allocation failure preserves old file contents and the dirty editor buffer;
no half-created name is published. Leaving the editor releases the buffer and
all inode references. State remains volatile and disappears when QEMU exits.

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
`reap all` releases the four jobs' resources. The final `mem` should return
to the baseline printed before `demo`. The mounted filesystem and its
bootstrap files continue to own allocator pages even when no processes live.

Jobs advance the scheduler clock once per step, then yield or check whether
rescheduling is required. Time is software-fed; hardware timer preemption is
still pending. The shell waits while `run` executes jobs and returns when
there are no runnable jobs. Creating or waking a job does not run it until
you enter `run`.

## Basic operations

| Commands | What to inspect |
| --- | --- |
| `ps`, `show PID` | State, exit status, registers, mappings, cwd inode number and occupied fds |
| `queues`, `boost`, `tick [N]` | Ready queues, priority budgets and test clock |
| `spawn cpu [N]` | Work that checks for rescheduling after each step |
| `spawn yield [N]` | Work that voluntarily yields after every step |
| `spawn sleep [N]`, `wake PID\|all` | A blocked continuation and its later resumption |
| `kill PID\|all`, `run`, `reap PID\|all` | Deferred termination and resource reclamation |
| `exit PID [CODE]`, `reap PID` | Explicit exit of an inactive process |
| `mem`, `alloc ORDER`, `free ID\|all` | Buddy allocation, exhaustion, release and coalescing |
| `image PATH`, `fork PID`, `exec PID PATH [args...]` | File-backed image creation, page copying and replacement |
| `pwd`, `cd [PATH]`, `mkdir PATH`, `touch PATH` | Shell cwd and ordinary directory/file creation |
| `ls [PATH]`, `cat PATH`, `rm PATH` | Browse, display and remove shell-relative entries |
| `editor PATH`, `edit PATH` | Interactive line editor |
| `mkdir PID PATH`, `chdir PID PATH`, `unlink PID PATH` | Namespace operations relative to that process's cwd |
| `open PID PATH MODE` | Open a regular file using `r`, `w`, `rw`, `create` or `truncate` |
| `close PID FD`, `dup PID FD` | Close an fd or duplicate its shared open cursor |
| `read PID FD N`, `write PID FD TEXT` | Read up to 128 bytes or write one whitespace-free token |
| `echo [words...]` | Basic command parsing and console output |

Use the PID or block ID printed by the creation command. IDs continue
increasing after objects are reclaimed. Jobs default to six steps; accepted
counts are 1–1024. Allocation orders are 0–11, with sizes of
`4096 * 2^ORDER` bytes. The shell manages an 8 MiB buddy arena after the kernel,
BSS and boot stack; this is a subset of QEMU's 128 MiB RAM. Up to 16 manual
blocks and 32 processes can be tracked. Allocation success depends on current
free blocks: the mounted filesystem occupies part of the arena, so `alloc 11`
cannot reserve the entire 8 MiB. Record `mem` before an experiment and compare
against that baseline after cleanup; namespace entries and file contents stay
allocated until removed. Zombies release cwd and descriptors at exit but
retain user mappings and kernel stacks until reaped.

For example, create a sleeping job with `spawn sleep 2`, then enter `run`.
Use its printed PID with `show PID` and `kill PID`. Another `run` processes
the pending termination without resuming the killed callback. `show PID`
then reports a zombie with exit status -1; `reap PID` frees its stack.

For user-image lifecycle testing, use `image init`, `fork PID`, and
`exec CHILD_PID /alt hello world`. `init` and `alt` are ordinary executable
files installed in the RAM root. `image PATH` resolves relative to root;
`exec PID PATH` resolves relative to that process's cwd. Inspect both processes
with `show`: fork copies their backing pages and retains cwd and fd references;
exec loads the alternate file with two arguments while preserving cwd, fds and
their offsets. These images are saved software mappings: user-mode execution,
hardware address-space switching and trap handling are not implemented yet.
`run` reports an unsupported-operation error when it selects a live user
image. Enter `kill all`, `run`, and `reap all` to clear these test images
before continuing callable scheduling tests.

## File descriptors and namespace walkthrough

Each process has 16 descriptors, numbered 0–15, initially all empty. The lowest
available number is returned by open or dup; 0, 1 and 2 have no automatic
console endpoints. `r`, `w` and `rw` open an existing regular file without
changing its length. `create` opens for reading and writing, creating the file
if missing. `truncate` opens an existing file for reading and writing and
clears its contents. Independent opens have independent offsets; dup and fork
share the original open description and therefore its offset. Closing one
alias leaves the others usable.

Start a fresh shell for the exact PIDs below. In an existing session,
substitute the PIDs printed by `image` and `fork`:

```text
mem
image init
mkdir 1 /work
chdir 1 /work
open 1 note create
write 1 0 abcdef
open 1 note r
fork 1
read 1 1 2
read 2 1 2
close 1 1
unlink 1 note
read 2 1 2
show 2
exit 1
reap 1
chdir 2 /
unlink 2 /work
exit 2
reap 2
mem
```

The reads return `ab`, `cd` and `ef`: parent and child fd 1 share an offset,
and the child can read after the parent closes its alias and unlinks the name.
The writer on fd 0 is a separate open description. `show 2` displays the
retained cwd inode number and fds 0 and 1. Exiting each process closes its
descriptors and releases cwd; reaping frees its mappings and stack. Removing
the test file and directory lets the last `mem` return to its initial baseline.
All filesystem state is volatile and disappears when QEMU exits.

The parser accepts up to 255 characters and 12 tokens per line, with quoted
words and backslash escapes. `exec` accepts up to eight argument words. `write`
accepts one token (which may be quoted), without an automatic newline; `read`
accepts a count from 0 to 128. Command chaining, pipes, expansion and redirection
are not provided.

## Automated verification

```sh
make stage-test
```

This runs six historical host suites, the freestanding QEMU kernel checks, and
`tests/qemu_shell_test.py`. The Python harness drives a real QEMU UART and
performs 384 checks for job execution, sleep/wakeup, kill/reap, file-backed
fork/exec, cwd, shared descriptors, unlink while open, shell navigation,
creation/listing/removal, quotes, editor insert/replace/delete/save/discard,
capacity limits, failed-save rollback, exhaustion, malformed input and memory
recovery. It prints `SHELL OK` and
saves the serial transcript to `build/shell_test.log`. Run `make test-shell`
to execute just the shell checks.

The kernel's `tests/proc_fs_test.c` reports `PROC FS OK` after checking image
loading, descriptor and cwd ownership, system-call user-buffer validation,
allocation-failure rollback and full allocator recovery. New kernel tests use
project headers and implementations; the kernel builds with `-nostdinc`,
`-ffreestanding`, `-fno-builtin` and `-nostdlib`.

`make` builds both `build/buddy.elf` and `build/shell.elf`. Regenerate clangd's
compilation database after source or build changes with:

```sh
bear -- make -B all test
```
