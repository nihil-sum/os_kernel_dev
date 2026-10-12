# RAM inode system

The current filesystem is volatile and uses only the kernel's own modules.
Inodes, directory entries and file blocks come from `kalloc`; membership and
ownership lists use `list.h`; copies and zeroing use the kernel C loops in
`kstring.h`. There is no block device or disk cache yet. `make_filesystem`
constructs an empty RAM root, and does not produce or format a disk image.

`src/fs/inode.c` implements inode lifetime, file data and directory entries.
`src/fs/fs.c` implements path lookup and open-file cursors. Public declarations
are in `src/include/fs.h`. All APIs require serialization, including with
other users of kalloc/buddy. The current cooperative single-hart runtime is
compatible with this rule; there are no filesystem locks yet.

## Initialization and ownership

Initialize buddy and kalloc first. Zero-initialize a `struct fs`, keep it at a
stable address, then call `fs_init(&fs, device_id)` or `make_filesystem(&fs)`
(device ID zero). Failure leaves its state untouched. Reinitialization of a
mounted filesystem returns `FS_EBUSY`.

Each filesystem owns a list of inodes. `inum` starts at 1 for the root and
increases without reuse for that mount; failed create operations can leave
gaps. Identity is the owning filesystem plus `inum`. `dev` is a caller-supplied
label, not a device-driver handle. Callers must not modify inode/list fields.

- `ref` counts held kernel references, including open handles. Successful
  alloc/get/create/lookup returns exactly one reference in its output pointer.
  `inode_ref` duplicates a held reference; `inode_put` releases one.
- `nlink` counts directory names. A directory entry owns a link rather than
  a reference. The root has one special mount link. Synthetic `.` and `..`
  do not increment counts; these are not POSIX directory link-count rules.
- At `ref == 0 && nlink == 0`, the inode and all its data are freed. A linked
  inode survives without references. An unlinked file survives while open.
- `fs_destroy` refuses any remaining references, including open handles. Once
  all references are released it frees the whole namespace, including nested
  directories and multiply linked files, without recursive stack use.

All APIs accepting an inode pointer require a live held reference. `inode_get`
can obtain one by number. Never access a pointer after its last reference is
released; look it up again. Output pointers remain untouched on errors.

## File data

Regular files have explicit-offset `inode_read`, `inode_write` and
`inode_truncate`. Reads/writes return a byte count or a negative `fs_error`;
EOF returns zero. Other APIs return `FS_OK` or a negative error. Direct I/O
rejects directory inodes.

Files use sparse 512-byte blocks, with a maximum logical length of 1 MiB.
Each data record contains a block index, an embedded list node and the bytes.
Unallocated ranges read as zero. A write beyond EOF allocates only the blocks
it touches, and the gap stays zero. Writes reserve all missing blocks before
changing any data. Exhaustion frees those reservations and leaves existing
contents, size, block list and open-file offset intact.

Truncation can shrink or grow. Shrinking frees complete discarded blocks and
zeroes the discarded tail of the last retained block. Growing changes logical
size without allocating; reads of new ranges return zero, including after a
shrink/regrow sequence. Checked bounds prevent offset/length overflow. Buffers
are trusted kernel memory and must not overlap filesystem-owned storage;
future user syscalls must use the existing checked process-copy boundary.

Blocks, inode numbers and directory names currently use linear list searches.
This favors a small implementation; indexing and disk block mapping are later
improvements. A failed create may consume an inode number, but cannot leave
a directory entry or leaked inode behind.

## Namespace and open cursors

`inode_create(parent, name, type, &inode)` allocates and links a new inode.
`inode_alloc` also supports unnamed files; `dir_link` attaches them later.
`dir_lookup` returns a reference, and `dir_unlink` removes a name. File hard
links are supported within one filesystem. A directory can only be attached
when empty and unlinked; only attached directories accept entries. These
rules prevent directory cycles. Nonempty directories cannot be unlinked.
On unlink, an empty directory's parent is cleared; `..` then returns ENOENT.

Names have at most 63 bytes. Empty names, slash-containing names and attempts
to mutate `.` or `..` are rejected. `fs_lookup` supports absolute paths,
relative paths from a held start directory, repeated slashes and `.`/`..`.
Paths have at most 1024 bytes. A trailing slash requires a directory. Lookup
releases temporary references on both success and error.

`dir_read(dir, index, &entry)` returns a metadata snapshot for an explicit
directory entry in insertion order: 1 for an entry, 0 at EOF, or a negative
error. It transfers no inode reference and omits synthetic `.`/`..`; outputs
are unchanged at EOF/error. Indices may shift after namespace mutations, so
enumeration requires the same serialization as other filesystem operations.
`fs_getpath(fs, dir, buffer, size)` builds a canonical directory path without
allocation or recursion, checks the 1024-byte limit and buffer capacity, and
rejects detached directories. These APIs support shell `ls` and `pwd`.

`struct file` is an initially zeroed, caller-owned open handle. `file_open`
retains a regular inode and starts an independent cursor at zero; mode is
`FILE_READ`, `FILE_WRITE`, or both. `file_read`/`file_write` advance it by the
successful byte count. `file_close` releases its reference and clears the
handle. Do not copy a live handle. The process layer in `proc_fs.h` now wraps
these handles in shared open descriptions for descriptor tables and fork/dup.

```c
#include "fs.h"

/* kalloc is already initialized. Check every returned status in real code. */
struct fs filesystem = {0};
struct inode *root, *note;
struct file handle = {0};
fs_init(&filesystem, 0);
inode_get(&filesystem, 1, &root);
inode_create(root, "note", FILE_NODE, &note);
file_open(&handle, note, FILE_READ | FILE_WRITE);
file_write(&handle, "hello", 5);
inode_put(note);
dir_unlink(root, "note"); /* handle still owns a reference */
file_close(&handle);      /* last reference: inode/data are reclaimed */
inode_put(root);
fs_destroy(&filesystem);
```

The old unused `write`/`readc` one-page experiment and header-local fd bitmap
macros were replaced by these APIs. Inode identity uses `inum` rather than
duplicate `ino`/`inum` fields. File offsets live in open handles, not inodes.
The draft device major/minor fields are deferred until device inodes exist.

## Validation and next layers

`tests/inode_test.c` has no standard headers or library dependencies. It runs
inside `make qemu`, reports `INODE OK`, and prints its failing line on error.
It covers references/links, independent cursors, unlink while open, nested
paths, invalid names, cross-filesystem links, sparse multi-block I/O,
truncation, overflow, allocation-failure rollback, and teardown of deep trees.
Directory snapshots and canonical paths also cover EOF, detached directories,
foreign mounts, deep traversal, exact path limits and small output buffers.
Each complete run verifies buddy-page recovery and an empty kalloc heap.
`make stage-test` also runs the existing host and UART shell regressions.

The process layer now provides descriptor/cwd ownership, file syscalls and
filesystem-backed exec; see [the process guide](../proc/proc.md). Remaining
filesystem layers include a block device and cache followed by persistent
inode/block formats, locking, permissions, device nodes, rename and journaling.
