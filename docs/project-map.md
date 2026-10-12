# Kernel project map and filesystem integration

Reviewed on 2026-10-12 and updated after inode/process integration.

| Area | Working implementation | Missing or draft |
| --- | --- | --- |
| Boot | RV64 QEMU virt entry, BSS clearing, linker layout, boot stack | Trap entry/return, timer interrupts, multiple harts |
| Memory | Buddy page allocation/coalescing; slab `kalloc`/`kfree` | Virtual memory/Sv39, allocator locking; old `mm.c` is an alternative experiment |
| Utilities | Intrusive doubly linked lists; kernel byte-loop `memset`/`memcpy`; callable descriptors and typed results | General kernel utility library |
| Processes | Page-backed image/stack ownership; filesystem exec; fork/dup shared descriptors; cwd references; exit/reap; file syscalls and checked user copying | Hardware user execution, wait, pipes, console descriptors, close-on-exec |
| Scheduling | Four-level MLFQ; cooperative RV64 stack switching; yield, sleep, wakeup | Interrupt-driven preemption, synchronization primitives |
| Console | Polled UART; line input; shell cwd, cd/mkdir/touch/ls/cat/rm and text editor; process/scheduler test commands | Device abstraction, user-mode shell |
| Filesystem | RAM inodes with ref/link ownership; directories/paths and enumeration; sparse I/O/truncation; open cursors | Persistent filesystem and device nodes |
| Storage | None | Block-device driver, block cache, disk layout, journal, persistent mount/format |
| Validation | Six host suites; freestanding inode/process-filesystem QEMU tests; 384 UART checks | Hardware user-mode/trap execution tests |

The kernel supplies its own types, headers and byte-copy routines and builds
with `-nostdinc` and `-nostdlib`. Historical host tests use libc only in their
test harness; new inode and process/filesystem checks execute inside QEMU
using project headers, kernel checks and UART output.

## Inode implementation plan

1. Define kernel primitive types/constants using C built-in types; migrate
   kernel includes and add a header for the existing kernel memory routines.
   Enforce the no-standard-headers/no-standard-library rule in the kernel build.
2. Implement a volatile RAM filesystem on `kalloc` and the intrusive lists.
   Use filesystem-local inode numbers, separate reference and namespace link
   counts, a mounted root, and reclamation when both counts reach zero.
3. Add fixed-size data blocks, explicit-offset reads/writes, sparse zero reads,
   truncation, checked limits, and rollback if a write cannot allocate all
   required blocks. Allocate metadata and data only through `kalloc`.
4. Implement named directory entries, create/lookup/link/unlink, bounded path
   traversal, and open-file cursors that retain inode references. Forbid
   directory hard links/cycles and unlinking nonempty directories.
5. Replace the unused filesystem drafts with the public API, implement the
   RAM-root initializer in `mkfs.c`, and document ownership and error behavior.
6. Add freestanding QEMU tests for namespace/identity, independent cursors,
   unlink while open, sparse and multi-block I/O, truncation, exhaustion and
   rollback, reference leaks, and complete allocator recovery. Run existing
   stage tests and verify that kernel ELF files have no unresolved symbols.

This stage deliberately provides an in-memory inode system. Persistent inodes
need the storage modules listed above first. File-descriptor syscalls and
process cwd/descriptor inheritance were the next integration layer after this
API, implemented below alongside fork/exec/exit ownership rules.

## Completed result

All six steps above are implemented. The kernel now includes the RAM inode
and directory layer, sparse file data and open-file cursors. See
[`fs.h`](../src/include/fs.h) and [the API guide](../src/fs/fs.md).
At the end of that stage, `make stage-test` passed all six host suites, the
QEMU smoke checks (including `INODE OK`), and 103 UART shell checks. Both kernel images build
without system headers or libraries and have no unresolved symbols. Storage,
process descriptors/cwd, user file syscalls and filesystem-backed exec remain
pending at the end of the inode stage; the process work below now implements
cwd, descriptors, file syscalls and filesystem-backed exec.

## Process integration completed

1. Process tables pin the filesystem root. New processes retain cwd; fork
   inherits that reference. Table destruction requires all processes reaped
   and the scheduler detached, then releases the mount reference.
2. Processes have 16 descriptor slots pointing to reference-counted open
   descriptions allocated through kalloc. Fork/dup share descriptions and
   offsets. Separate opens have independent offsets. Exit releases descriptors
   and cwd immediately; reap releases the process image and kernel stack.
3. Exec resolves inode paths relative to cwd. A small 40-byte versioned RV64
   flat-image header describes code/data/BSS/entry. The loader validates it,
   builds all replacement pages and arguments, then commits. Failure preserves
   the old image and filesystem ownership. The older registry initializer is
   retained for compatibility tests; the shell uses filesystem tables.
4. File and namespace syscall entry points validate user paths/buffers using
   the process copy boundary. Writes stage the user input before issuing a
   single inode write. Long exec paths/arguments use heap staging to fit the
   existing one-page kernel stack.
5. The shell installs executable files and exposes open/read/write/close/dup,
   mkdir/chdir/unlink, plus cwd/descriptor inspection. Its regression checks
   account for mounted filesystem pages and verify shared offsets and cleanup.
6. `make stage-test` passes with `PROC FS OK` and 134 shell checks. The new
   freestanding tests also force every page-allocation failure in create,
   fork and exec, checking reference/page recovery and the surviving image.

See [process design](../src/proc/proc.md) and
[process file APIs](../src/include/proc_fs.h). Remaining work includes storage
drivers and persistence, hardware user address spaces and trap entry/return,
wait, pipes, file/device permissions and console descriptor endpoints.

## Interactive shell completed

1. Retain an independent shell cwd at root, without reserving a process slot;
   implement cd/pwd on inode lookup and bounded canonical directory paths.
2. Implement mkdir/touch with checked parent lookup; touch preserves existing
   regular-file contents. Keep the existing mkdir PID PATH test interface.
3. Enumerate directory metadata through dir_read for ls. Add cat/rm to inspect
   and clean up files, plus quoted paths and escapes in the command parser.
4. Implement a 16 KiB heap-backed line editor with numbered output,
   append/insert/replace/delete, save/quit and discard. Existing-file save uses
   atomic inode writes before truncation; new-file save publishes only after
   writes succeed. Failed saves preserve old data and staged edits.
5. Verify navigation, type errors, quoted paths, editing, reopen/truncate,
   capacity bounds, cancellation, failed saves and full memory recovery over
   real QEMU UART. make stage-test passes all kernel/host suites and 384 shell
   checks. See the shell guide for a manual create/edit/list walkthrough.

This shell runs in the kernel on the boot stack. User-mode execution and disk
persistence remain the separate architecture/storage stages listed above.
