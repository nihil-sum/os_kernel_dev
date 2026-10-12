#include "proc_fs_test.h"
#include "proc_fs.h"
#include "syscall.h"
#include "fs.h"
#include "kstring.h"

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

/* Large objects live outside the kernel's one-page process stacks. */
static struct proc_table table;
static struct proc snapshot;
static struct fs filesystem;
static struct buddy_allocator *memory;
static struct inode *root;
static struct block reserved[256];
static u8_t code[BUDDY_PAGE_SIZE + 4];
static u8_t output[8192];
static u8_t input[1024];
static const u8_t data[] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3 };
static const u8_t tiny_code[] = { 0x73, 0, 0, 0 };
static const struct proc_image demo_image = {
	.name = "demo", .code = code, .code_size = sizeof(code),
	.data = data, .data_size = sizeof(data), .bss_size = 5000,
	.entry_offset = 2
};
static const struct proc_image tiny_image = {
	.name = "tiny", .code = tiny_code, .code_size = sizeof(tiny_code)
};

static ksize_t free_pages(void) {
	ksize_t result = 0;
	for (int order = 0; order <= BUDDY_MAX_ORDER; order++)
		result += memory->free_lists[order].len * ((ksize_t)1 << order);
	return result;
}

static int equal_bytes(const void *left, const void *right, ksize_t count) {
	const u8_t *a = left, *b = right;
	for (ksize_t i = 0; i < count; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

static int zero_bytes(const void *pointer, ksize_t count) {
	const u8_t *bytes = pointer;
	for (ksize_t i = 0; i < count; i++)
		if (bytes[i] != 0) return 0;
	return 1;
}

static ksize_t live_processes(void) {
	ksize_t result = 0;
	for (ksize_t i = 0; i < PROC_MAX; i++)
		if (table.entries[i].pstatus != UNUSED) result++;
	return result;
}

static int setup(void) {
	struct inode *bin, *work;
	CHECK(make_filesystem(&filesystem) == FS_OK);
	CHECK(inode_get(&filesystem, 1, &root) == FS_OK);
	CHECK(inode_create(root, "bin", DIR_NODE, &bin) == FS_OK);
	CHECK(inode_create(root, "work", DIR_NODE, &work) == FS_OK);
	CHECK(proc_image_install(bin, "demo", &demo_image) == 0);
	CHECK(proc_image_install(bin, "tiny", &tiny_image) == 0);
	CHECK(inode_put(bin) == FS_OK && inode_put(work) == FS_OK);
	CHECK(proc_table_init_fs(&table, memory, &filesystem) == 0);
	CHECK(fs_destroy(&filesystem) == FS_EBUSY);
	return 0;
}

static int destroy_process(struct proc *process) {
	u64_t pid = process->pid;
	CHECK(proc_exit(&table, process, 0) == 0);
	CHECK(process->cwd == NULL);
	for (ksize_t i = 0; i < PROC_FD_MAX; i++) CHECK(process->files[i] == NULL);
	if (table.current == process) CHECK(proc_deactivate(&table) == 0);
	CHECK(proc_reap(&table, pid) == 0);
	return 0;
}

static int teardown(void) {
	CHECK(live_processes() == 0);
	CHECK(inode_put(root) == FS_OK);
	root = NULL;
	/* The table retains its mount even when it has no processes. */
	CHECK(fs_destroy(&filesystem) == FS_EBUSY);
	CHECK(proc_table_destroy(&table) == 0);
	CHECK(fs_destroy(&filesystem) == FS_OK);
	return 0;
}

static int image_checks(void) {
	int result = setup();
	if (result != 0) return result;
	struct proc *process = NULL;
	CHECK(proc_create(&table, "/missing", &process) == PROC_ENOENT);
	CHECK(process == NULL && live_processes() == 0);
	CHECK(proc_create(&table, "/bin/demo", &process) == 0);
	CHECK(process->cwd == root && process->space.count == 5);
	CHECK(process->tf.pc == PROC_CODE_BASE + 2);
	CHECK(proc_copyin(process, output, PROC_CODE_BASE, sizeof(code)) == 0);
	CHECK(equal_bytes(output, code, sizeof(code)));
	u64_t data_va = PROC_CODE_BASE + 2 * BUDDY_PAGE_SIZE;
	CHECK(proc_copyin(process, output, data_va, sizeof(data) + 5000) == 0);
	CHECK(equal_bytes(output, data, sizeof(data)));
	CHECK(zero_bytes(output + sizeof(data), 5000));
	CHECK(proc_copyout(process, PROC_CODE_BASE, "x", 1) == PROC_EFAULT);
	CHECK(proc_table_destroy(&table) == PROC_EBUSY);
	CHECK(proc_chdir(&table, process, "/work") == 0);
	int fd = proc_open(&table, process, "kept", PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE);
	CHECK(fd == 0 && proc_write(&table, process, fd, "abc", 3) == 3);
	struct inode *cwd = process->cwd;
	struct proc_file *description = process->files[fd];
	struct inode *broken;
	CHECK(inode_create(root, "broken", FILE_NODE, &broken) == FS_OK);
	CHECK(inode_write(broken, "truncated", 0, 9) == 9);
	memcpy(&snapshot, process, sizeof(snapshot));
	ksize_t available = free_pages();
	CHECK(proc_exec(&table, process, "/broken", 0, NULL) == PROC_ENOEXEC);
	CHECK(equal_bytes(process, &snapshot, sizeof(snapshot)) && free_pages() == available);
	struct proc_exec_header header = {
		.magic = PROC_EXEC_MAGIC, .code_size = 4, .entry_offset = 0
	};
	CHECK(inode_truncate(broken, 0) == FS_OK);
	CHECK(inode_write(broken, &header, 0, sizeof(header)) == (i64_t)sizeof(header));
	available = free_pages();
	/* A correct header with no advertised code bytes is still malformed. */
	CHECK(proc_exec(&table, process, "/broken", 0, NULL) == PROC_ENOEXEC);
	CHECK(equal_bytes(process, &snapshot, sizeof(snapshot)) && free_pages() == available);
	header.code_size = U64_MAX;
	header.data_size = 1;
	CHECK(inode_write(broken, &header, 0, sizeof(header)) == (i64_t)sizeof(header));
	CHECK(proc_exec(&table, process, "/broken", 0, NULL) == PROC_ENOEXEC);
	CHECK(equal_bytes(process, &snapshot, sizeof(snapshot)));
	CHECK(inode_put(broken) == FS_OK);
	const char *arguments[] = { "tiny", "argument" };
	u64_t pid = process->pid;
	uptr_t stack = process->kstack.addr;
	CHECK(proc_exec(&table, process, "../bin/tiny", 2, arguments) == 0);
	CHECK(process->pid == pid && process->kstack.addr == stack);
	CHECK(process->cwd == cwd && process->files[fd] == description);
	CHECK(process->tf.pc == PROC_CODE_BASE && process->tf.x[PROC_A0] == 2);
	CHECK(process->tf.x[PROC_SP] % 16 == 0 && process->space.count == 2);
	u64_t argv[3];
	CHECK(proc_copyin(process, argv, process->tf.x[PROC_A1], sizeof(argv)) == 0);
	CHECK(argv[2] == 0);
	CHECK(proc_copyinstr(process, (char *)output, argv[0], sizeof(output)) == 0);
	CHECK(equal_bytes(output, "tiny", 5));
	CHECK(proc_copyinstr(process, (char *)output, argv[1], sizeof(output)) == 0);
	CHECK(equal_bytes(output, "argument", 9));
	CHECK(proc_write(&table, process, fd, "d", 1) == 1);
	struct inode *kept;
	CHECK(fs_lookup(&filesystem, cwd, "kept", &kept) == FS_OK);
	CHECK(inode_read(kept, output, 0, sizeof(output)) == 4);
	CHECK(equal_bytes(output, "abcd", 4) && inode_put(kept) == FS_OK);
	result = destroy_process(process);
	if (result != 0) return result;
	return teardown();
}

static int inheritance_checks(void) {
	int result = setup();
	if (result != 0) return result;
	struct proc *parent;
	CHECK(proc_create(&table, "/bin/tiny", &parent) == 0);
	CHECK(proc_chdir(&table, parent, "/work") == 0);
	CHECK(proc_open(&table, parent, "note", PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE) == 0);
	CHECK(proc_write(&table, parent, 0, "abcdef", 6) == 6);
	CHECK(proc_close(&table, parent, 0) == 0);
	CHECK(proc_open(&table, parent, "note", PROC_O_READ | PROC_O_WRITE) == 0);
	CHECK(proc_dup(&table, parent, 0) == 1);
	CHECK(proc_open(&table, parent, "note", PROC_O_READ) == 2);
	CHECK(parent->files[0] == parent->files[1]);
	CHECK(parent->files[0] != parent->files[2]);
	CHECK(proc_read(&table, parent, 0, output, 2) == 2 && equal_bytes(output, "ab", 2));
	CHECK(proc_read(&table, parent, 1, output, 2) == 2 && equal_bytes(output, "cd", 2));
	CHECK(proc_read(&table, parent, 2, output, 1) == 1 && output[0] == 'a');
	i64_t child_pid = proc_fork(&table, parent);
	CHECK(child_pid > 0);
	struct proc *child = proc_find(&table, (u64_t)child_pid);
	CHECK(child != NULL && child->parent == parent && child->cwd == parent->cwd);
	CHECK(child->files[0] == parent->files[0] && child->files[2] == parent->files[2]);
	CHECK(proc_read(&table, child, 0, output, 1) == 1 && output[0] == 'e');
	CHECK(proc_read(&table, parent, 1, output, 1) == 1 && output[0] == 'f');
	CHECK(proc_close(&table, parent, 0) == 0 && proc_close(&table, parent, 1) == 0);
	CHECK(proc_write(&table, child, 0, "g", 1) == 1);
	CHECK(proc_chdir(&table, parent, "/") == 0);
	CHECK(parent->cwd == root && child->cwd != root);
	CHECK(proc_open(&table, parent, "note", PROC_O_READ) == PROC_ENOENT);
	CHECK(proc_open(&table, child, "note", PROC_O_READ) == 3);
	struct inode *note;
	CHECK(fs_lookup(&filesystem, child->cwd, "note", &note) == FS_OK);
	u64_t inum = note->inum;
	CHECK(inode_put(note) == FS_OK);
	CHECK(proc_unlink(&table, child, "note") == 0);
	CHECK(proc_open(&table, child, "note", PROC_O_READ) == PROC_ENOENT);
	CHECK(proc_read(&table, child, 3, output, sizeof(output)) == 7);
	CHECK(equal_bytes(output, "abcdefg", 7));
	CHECK(inode_get(&filesystem, inum, &note) == FS_OK && note->nlink == 0);
	CHECK(inode_put(note) == FS_OK);
	/* Exit drops references immediately, before image and stack reclamation. */
	u64_t parent_pid = parent->pid;
	CHECK(proc_exit(&table, parent, 17) == 0 && parent->cwd == NULL);
	for (ksize_t i = 0; i < PROC_FD_MAX; i++) CHECK(parent->files[i] == NULL);
	CHECK(child->parent == NULL && proc_table_destroy(&table) == PROC_EBUSY);
	CHECK(proc_read(&table, child, 2, output, 2) == 2 && equal_bytes(output, "bc", 2));
	CHECK(proc_reap(&table, parent_pid) == 0);
	CHECK(proc_exit(&table, child, 0) == 0 && child->cwd == NULL);
	for (ksize_t i = 0; i < PROC_FD_MAX; i++) CHECK(child->files[i] == NULL);
	note = NULL;
	CHECK(inode_get(&filesystem, inum, &note) == FS_ENOENT && note == NULL);
	CHECK(proc_table_destroy(&table) == PROC_EBUSY);
	CHECK(proc_reap(&table, (u64_t)child_pid) == 0);
	return teardown();
}

static int descriptor_checks(void) {
	int result = setup();
	if (result != 0) return result;
	struct proc *process;
	CHECK(proc_create(&table, "/bin/tiny", &process) == 0);
	CHECK(proc_open(&table, process, "/held", PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE) == 0);
	CHECK(proc_write(&table, process, 0, "keep", 4) == 4);
	for (unsigned int i = 1; i < PROC_FD_MAX; i++)
		CHECK(proc_open(&table, process, "/held", PROC_O_READ) == (int)i);
	CHECK(proc_dup(&table, process, 0) == PROC_EMFILE);
	CHECK(proc_open(&table, process, "/must-not-create", PROC_O_WRITE | PROC_O_CREATE) == PROC_EMFILE);
	CHECK(proc_open(&table, process, "/held", PROC_O_WRITE | PROC_O_TRUNC) == PROC_EMFILE);
	struct inode *found = NULL;
	CHECK(fs_lookup(&filesystem, root, "/must-not-create", &found) == FS_ENOENT && found == NULL);
	CHECK(fs_lookup(&filesystem, root, "/held", &found) == FS_OK);
	CHECK(found->size == 4 && inode_read(found, output, 0, 4) == 4);
	CHECK(equal_bytes(output, "keep", 4) && inode_put(found) == FS_OK);
	for (unsigned int i = 0; i < PROC_FD_MAX; i++)
		CHECK(proc_close(&table, process, (int)i) == 0);
	CHECK(proc_close(&table, process, 0) == PROC_EBADF);
	CHECK(proc_dup(&table, process, -1) == PROC_EBADF);
	CHECK(proc_open(&table, process, "/held", PROC_O_READ | PROC_O_TRUNC) == PROC_EINVAL);
	CHECK(proc_open(&table, process, "/held", PROC_O_WRITE | PROC_O_CREATE | PROC_O_EXCL) == PROC_EEXIST);
	CHECK(proc_open(&table, process, "/held/", PROC_O_READ) == PROC_ENOTDIR);
	CHECK(proc_open(&table, process, "/work", PROC_O_READ) == PROC_EISDIR);
	CHECK(proc_chdir(&table, process, "/held") == PROC_ENOTDIR && process->cwd == root);
	CHECK(proc_mkdir(&table, process, "/new") == 0);
	CHECK(proc_mkdir(&table, process, "/new") == PROC_EEXIST);
	CHECK(proc_open(&table, process, "/new/inside", PROC_O_WRITE | PROC_O_CREATE) == 0);
	CHECK(proc_read(&table, process, 0, output, 1) == PROC_EPERM);
	CHECK(proc_close(&table, process, 0) == 0);
	CHECK(proc_unlink(&table, process, "/new") == PROC_ENOTEMPTY);
	CHECK(proc_unlink(&table, process, "/new/inside") == 0);
	CHECK(proc_unlink(&table, process, "/new/") == 0);
	CHECK(proc_unlink(&table, process, "/") == PROC_EPERM);
	CHECK(proc_unlink(&table, process, ".") == PROC_EPERM);
	CHECK(proc_open(&table, process, "/held", PROC_O_WRITE | PROC_O_TRUNC) == 0);
	CHECK(proc_close(&table, process, 0) == 0);
	CHECK(fs_lookup(&filesystem, root, "/held", &found) == FS_OK);
	CHECK(found->size == 0 && inode_put(found) == FS_OK);
	result = destroy_process(process);
	if (result != 0) return result;
	return teardown();
}

static enum syscall_action dispatch(struct proc *process, u64_t number,
				    u64_t a0, u64_t a1, u64_t a2) {
	process->tf.x[PROC_A7] = number;
	process->tf.x[PROC_A0] = a0;
	process->tf.x[PROC_A1] = a1;
	process->tf.x[PROC_A2] = a2;
	return syscall_dispatch(&table);
}

static int syscall_checks(void) {
	int result = setup();
	if (result != 0) return result;
	struct proc *process;
	CHECK(proc_create(&table, "/bin/demo", &process) == 0);
	CHECK(proc_activate(&table, process) == 0);
	u64_t path_va = PROC_STACK_BASE + 16;
	u64_t buffer_va = PROC_STACK_BASE + 256;
	CHECK(proc_copyout(process, path_va, "/sysdir", 8) == 0);
	CHECK(dispatch(process, SYS_MKDIR, path_va, 0, 0) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 0);
	CHECK(dispatch(process, SYS_CHDIR, path_va, 0, 0) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 0 && process->cwd != root);
	CHECK(proc_copyout(process, path_va, "io", 3) == 0);
	CHECK(dispatch(process, SYS_OPEN, path_va, PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE, 0) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 0);
	CHECK(proc_copyout(process, buffer_va, input, 600) == 0);
	CHECK(dispatch(process, SYS_WRITE, 0, buffer_va, 600) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 600);
	CHECK(dispatch(process, SYS_CLOSE, 0, 0, 0) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 0);
	CHECK(dispatch(process, SYS_OPEN, path_va, PROC_O_READ | PROC_O_WRITE, 0) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 0);
	CHECK(dispatch(process, SYS_DUP, 0, 0, 0) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 1);
	CHECK(process->files[0] == process->files[1]);
	CHECK(dispatch(process, SYS_CLOSE, 1, 0, 0) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 0);
	CHECK(dispatch(process, SYS_READ, U64_MAX, buffer_va, 4) == SYSCALL_RETURN);
	CHECK((i64_t)process->tf.x[PROC_A0] == PROC_EBADF);
	CHECK(dispatch(process, SYS_READ, 0, PROC_STACK_TOP - 1, 2) == SYSCALL_RETURN);
	CHECK((i64_t)process->tf.x[PROC_A0] == PROC_EFAULT);
	CHECK(dispatch(process, SYS_READ, 0, PROC_CODE_BASE, 2) == SYSCALL_RETURN);
	CHECK((i64_t)process->tf.x[PROC_A0] == PROC_EFAULT);
	CHECK(dispatch(process, SYS_READ, 0, buffer_va, 4) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 4);
	CHECK(proc_copyin(process, output, buffer_va, 4) == 0 && equal_bytes(output, input, 4));
	CHECK(dispatch(process, SYS_WRITE, 0, PROC_STACK_TOP - 1, 2) == SYSCALL_RETURN);
	CHECK((i64_t)process->tf.x[PROC_A0] == PROC_EFAULT);
	CHECK(dispatch(process, SYS_READ, 0, buffer_va, 4) == SYSCALL_RETURN);
	CHECK(process->tf.x[PROC_A0] == 4);
	CHECK(proc_copyin(process, output, buffer_va, 4) == 0 && equal_bytes(output, input + 4, 4));
	struct inode *file;
	CHECK(fs_lookup(&filesystem, process->cwd, "io", &file) == FS_OK);
	CHECK(file->size == 600 && inode_read(file, output, 0, 600) == 600);
	CHECK(equal_bytes(output, input, 600) && inode_put(file) == FS_OK);
	/* The executable pathname exceeds the old process-name buffer length. */
	static const char executable[] = "/bin/../bin/../bin/../bin/../bin/tiny";
	CHECK(sizeof(executable) > PROC_NAME_MAX);
	CHECK(proc_copyout(process, path_va, executable, sizeof(executable)) == 0);
	CHECK(proc_copyout(process, buffer_va, "from-fs", 8) == 0);
	u64_t user_argv[] = { buffer_va, 0 };
	u64_t argv_va = PROC_STACK_BASE + 128;
	CHECK(proc_copyout(process, argv_va, user_argv, sizeof(user_argv)) == 0);
	struct inode *cwd = process->cwd;
	struct proc_file *description = process->files[0];
	CHECK(dispatch(process, SYS_EXEC, path_va, argv_va, 0) == SYSCALL_EXEC);
	CHECK(process->tf.x[PROC_A0] == 1 && process->cwd == cwd && process->files[0] == description);
	CHECK(proc_copyin(process, user_argv, process->tf.x[PROC_A1], sizeof(user_argv)) == 0);
	CHECK(user_argv[1] == 0);
	CHECK(proc_copyinstr(process, (char *)output, user_argv[0], sizeof(output)) == 0);
	CHECK(equal_bytes(output, "from-fs", 8));
	CHECK(dispatch(process, SYS_READ, 0, buffer_va, 2) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 2);
	CHECK(proc_copyin(process, output, buffer_va, 2) == 0 && equal_bytes(output, input + 8, 2));
	CHECK(proc_copyout(process, path_va, "io", 3) == 0);
	CHECK(dispatch(process, SYS_UNLINK, path_va, 0, 0) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 0);
	CHECK(dispatch(process, SYS_READ, 0, buffer_va, 2) == SYSCALL_RETURN && process->tf.x[PROC_A0] == 2);
	CHECK(proc_copyin(process, output, buffer_va, 2) == 0 && equal_bytes(output, input + 10, 2));
	u64_t pid = process->pid;
	CHECK(dispatch(process, SYS_EXIT, 23, 0, 0) == SYSCALL_EXIT);
	CHECK(process->pstatus == ZOMBIE && process->exit_status == 23 && process->cwd == NULL);
	for (ksize_t i = 0; i < PROC_FD_MAX; i++) CHECK(process->files[i] == NULL);
	CHECK(proc_reap(&table, pid) == PROC_EBUSY);
	CHECK(proc_deactivate(&table) == 0 && proc_reap(&table, pid) == 0);
	return teardown();
}

static int exhaustion_checks(void) {
	int result = setup();
	if (result != 0) return result;
	for (unsigned int operation = 0; operation < 3; operation++) {
		/* demo create: stack + 5 image pages; tiny fork: stack + 2;
		 * demo exec: 5 replacement pages, preserving the old image. */
		ksize_t needed = operation == 0 ? 6 : (operation == 1 ? 3 : 5);
		for (ksize_t available = 0; available < needed; available++) {
			struct proc *process = NULL;
			if (operation != 0) {
				CHECK(proc_create(&table, "/bin/tiny", &process) == 0);
				CHECK(proc_chdir(&table, process, "/work") == 0);
				CHECK(proc_open(&table, process, "held", PROC_O_READ | PROC_O_WRITE | PROC_O_CREATE) == 0);
				memcpy(&snapshot, process, sizeof(snapshot));
			}
			u32_t root_refs = root->ref;
			ksize_t count = 0;
			while (free_pages() > available) {
				CHECK(count < sizeof(reserved) / sizeof(reserved[0]));
				CHECK(buddy_alloc(memory, 0, &reserved[count++]) == BUDDY_OK);
			}
			if (operation == 0) {
				CHECK(proc_create(&table, "/bin/demo", &process) == PROC_ENOMEM);
				CHECK(process == NULL && live_processes() == 0);
			} else if (operation == 1) {
				CHECK(proc_fork(&table, process) == PROC_ENOMEM);
			} else {
				CHECK(proc_exec(&table, process, "/bin/demo", 0, NULL) == PROC_ENOMEM);
			}
			CHECK(free_pages() == available && root->ref == root_refs);
			if (process != NULL) {
				CHECK(live_processes() == 1 && equal_bytes(process, &snapshot, sizeof(snapshot)));
				CHECK(proc_copyin(process, output, PROC_CODE_BASE, sizeof(tiny_code)) == 0);
				CHECK(equal_bytes(output, tiny_code, sizeof(tiny_code)));
			}
			for (ksize_t i = 0; i < count; i++) CHECK(buddy_free(memory, &reserved[i]) == BUDDY_OK);
			if (process != NULL) {
				result = destroy_process(process);
				if (result != 0) return result;
			}
		}
	}
	return teardown();
}

int proc_fs_test(struct buddy_allocator *allocator) {
	memory = allocator;
	ksize_t initial_pages = free_pages();
	for (ksize_t i = 0; i < sizeof(code); i++) code[i] = (u8_t)(i * 37 + 9);
	for (ksize_t i = 0; i < sizeof(input); i++) input[i] = (u8_t)(i * 13 + 7);
	int (*checks[])(void) = {
		image_checks, inheritance_checks, descriptor_checks,
		syscall_checks, exhaustion_checks
	};
	for (ksize_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		int result = checks[i]();
		if (result != 0) return result;
		CHECK(free_pages() == initial_pages);
	}
	return 0;
}
