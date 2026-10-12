#include "inode_test.h"
#include "fs.h"
#include "kalloc.h"
#include "kstring.h"

#define CHECK(expression) do { if (!(expression)) return __LINE__; } while (0)

static u8_t input[8192];
static u8_t output[8192];
static struct block reserved[256];

static ksize_t free_pages(struct buddy_allocator *allocator) {
	ksize_t count = 0;
	for (int order = 0; order <= BUDDY_MAX_ORDER; order++)
		count += allocator->free_lists[order].len * ((ksize_t)1 << order);
	return count;
}

static int equal_bytes(const u8_t *a, const u8_t *b, ksize_t count) {
	for (ksize_t i = 0; i < count; i++)
		if (a[i] != b[i]) return 0;
	return 1;
}

static int zero_bytes(const u8_t *bytes, ksize_t count) {
	for (ksize_t i = 0; i < count; i++)
		if (bytes[i] != 0) return 0;
	return 1;
}

static int namespace_checks(void) {
	struct fs fs = {0}, other = {0};
	struct inode *root, *dir, *file, *found, *foreign;
	CHECK(fs_init(NULL, 0) == FS_EINVAL);
	CHECK(make_filesystem(&fs) == FS_OK);
	CHECK(fs_init(&fs, 9) == FS_EBUSY && fs.dev == 0);
	CHECK(inode_get(&fs, 1, &root) == FS_OK);
	CHECK(root == fs.root && root->ref == 1 && root->nlink == 1);
	CHECK(root->type == DIR_NODE && root->parent == root);
	CHECK(inode_create(root, "dir", DIR_NODE, &dir) == FS_OK);
	CHECK(inode_create(dir, "file", FILE_NODE, &file) == FS_OK);
	u64_t inum = file->inum;
	CHECK(file->nlink == 1 && file->ref == 1 && file->size == 0);
	CHECK(dir_link(root, "alias", file) == FS_OK && file->nlink == 2);
	struct fs_dirent entry = {0};
	CHECK(dir_read(root, 0, &entry) == 1 && entry.type == DIR_NODE);
	CHECK(entry.inum == dir->inum && equal_bytes((u8_t *)entry.name, (const u8_t *)"dir", 4));
	CHECK(dir_read(root, 1, &entry) == 1 && entry.inum == file->inum && entry.size == 0);
	CHECK(equal_bytes((u8_t *)entry.name, (const u8_t *)"alias", 6));
	CHECK(dir_read(root, U64_MAX, &entry) == 0 && entry.inum == file->inum);
	CHECK(dir_read(file, 0, &entry) == FS_ENOTDIR && entry.inum == file->inum);
	CHECK(dir_read(NULL, 0, &entry) == FS_EINVAL);
	CHECK(dir_read(root, 0, NULL) == FS_EINVAL);
	char path[32];
	CHECK(fs_getpath(&fs, root, path, 2) == FS_OK && path[0] == '/' && path[1] == 0);
	CHECK(fs_getpath(&fs, root, path, 1) == FS_ENAMETOOLONG);
	CHECK(fs_getpath(&fs, dir, path, 5) == FS_OK);
	CHECK(equal_bytes((u8_t *)path, (const u8_t *)"/dir", 5));
	CHECK(fs_getpath(&fs, dir, path, 4) == FS_ENAMETOOLONG);
	CHECK(fs_getpath(&fs, file, path, sizeof(path)) == FS_ENOTDIR);
	CHECK(fs_getpath(&fs, dir, NULL, sizeof(path)) == FS_EINVAL);
	CHECK(dir_lookup(root, "alias", &found) == FS_OK && found == file);
	CHECK(inode_put(found) == FS_OK);
	CHECK(inode_put(file) == FS_OK); /* Namespace links keep ref == 0 alive. */
	CHECK(inode_get(&fs, inum, &file) == FS_OK && file->nlink == 2 && file->ref == 1);
	CHECK(fs_lookup(&fs, NULL, "//dir/./../dir//file", &found) == FS_OK);
	CHECK(found == file && inode_put(found) == FS_OK);
	CHECK(fs_lookup(&fs, dir, "../alias", &found) == FS_OK);
	CHECK(found == file && inode_put(found) == FS_OK);
	CHECK(fs_lookup(&fs, NULL, "/../../", &found) == FS_OK);
	CHECK(found == root && inode_put(found) == FS_OK);
	CHECK(fs_lookup(&fs, root, "dir/", &found) == FS_OK);
	CHECK(found == dir && inode_put(found) == FS_OK);
	found = root;
	CHECK(fs_lookup(&fs, NULL, "/dir/file/", &found) == FS_ENOTDIR && found == root);
	CHECK(fs_lookup(&fs, NULL, "/dir/file/..", &found) == FS_ENOTDIR);
	CHECK(fs_lookup(&fs, NULL, "relative", &found) == FS_EINVAL);
	CHECK(fs_lookup(&fs, root, "", &found) == FS_EINVAL);
	CHECK(fs_lookup(&fs, root, "missing/child", &found) == FS_ENOENT);
	CHECK(dir_lookup(file, ".", &found) == FS_ENOTDIR);
	CHECK(inode_create(root, "dir", FILE_NODE, &found) == FS_EEXIST && found == root);
	CHECK(dir_link(root, "dir2", dir) == FS_EPERM);
	CHECK(dir_link(dir, "cycle", root) == FS_EPERM);
	CHECK(dir_unlink(root, "dir") == FS_ENOTEMPTY);
	CHECK(dir_unlink(root, ".") == FS_EPERM);
	CHECK(dir_unlink(root, "..") == FS_EPERM);
	CHECK(inode_create(root, "", FILE_NODE, &found) == FS_EINVAL);
	CHECK(inode_create(root, "a/b", FILE_NODE, &found) == FS_EINVAL);
	CHECK(inode_create(root, ".", FILE_NODE, &found) == FS_EPERM);
	CHECK(inode_create(root, "bad-type", (enum inode_type)7, &found) == FS_EINVAL);
	CHECK(fs_init(&other, 7) == FS_OK);
	CHECK(inode_get(&other, 1, &foreign) == FS_OK && foreign->dev == 7);
	CHECK(dir_link(root, "cross-device", foreign) == FS_EXDEV);
	CHECK(fs_lookup(&fs, foreign, ".", &found) == FS_EXDEV);
	CHECK(fs_getpath(&fs, foreign, path, sizeof(path)) == FS_EXDEV);
	CHECK(inode_put(foreign) == FS_OK && fs_destroy(&other) == FS_OK);

	struct file writer = {0}, reader = {0}, denied = {0};
	CHECK(file_open(&denied, root, FILE_READ) == FS_EISDIR);
	CHECK(file_open(&denied, file, 0) == FS_EINVAL);
	CHECK(file_open(&writer, file, FILE_WRITE) == FS_OK);
	CHECK(file_open(&reader, file, FILE_READ) == FS_OK);
	CHECK(file_open(&writer, file, FILE_WRITE) == FS_EBUSY);
	CHECK(file_write(&writer, "hello", 5) == 5 && writer.offset == 5);
	CHECK(reader.offset == 0 && file->size == 5);
	CHECK(file_read(&writer, output, 1) == FS_EPERM && writer.offset == 5);
	CHECK(file_write(&reader, "x", 1) == FS_EPERM && reader.offset == 0);
	CHECK(dir_unlink(root, "alias") == FS_OK && file->nlink == 1);
	CHECK(dir_unlink(dir, "file") == FS_OK && file->nlink == 0);
	CHECK(inode_put(file) == FS_OK);
	CHECK(file_read(&reader, output, sizeof(output)) == 5);
	CHECK(equal_bytes(output, (const u8_t *)"hello", 5));
	CHECK(file_read(&reader, output, 1) == 0 && reader.offset == 5);
	CHECK(file_write(&writer, "!", 1) == 1);
	CHECK(file_read(&reader, output, 1) == 1 && output[0] == '!');
	CHECK(inode_get(&fs, inum, &found) == FS_OK && found == writer.inode);
	CHECK(inode_put(found) == FS_OK);
	CHECK(file_close(&reader) == FS_OK);
	CHECK(file_close(&reader) == FS_EINVAL);
	CHECK(inode_put(root) == FS_OK && inode_put(dir) == FS_OK);
	CHECK(fs_destroy(&fs) == FS_EBUSY); /* Only writer holds a reference. */
	CHECK(file_close(&writer) == FS_OK);
	found = NULL;
	CHECK(inode_get(&fs, inum, &found) == FS_ENOENT && found == NULL);
	CHECK(inode_get(&fs, 1, &root) == FS_OK);
	CHECK(dir_lookup(root, "dir", &dir) == FS_OK);
	CHECK(dir_unlink(root, "dir") == FS_OK && dir->parent == NULL);
	CHECK(fs_getpath(&fs, dir, path, sizeof(path)) == FS_ENOENT);
	CHECK(dir_lookup(dir, "..", &found) == FS_ENOENT);
	CHECK(inode_create(dir, "detached", FILE_NODE, &found) == FS_EPERM);
	CHECK(dir_link(root, "reattached", dir) == FS_OK && dir->parent == root);
	CHECK(fs_getpath(&fs, dir, path, sizeof(path)) == FS_OK);
	CHECK(equal_bytes((u8_t *)path, (const u8_t *)"/reattached", 12));
	CHECK(dir_unlink(root, "reattached") == FS_OK);
	CHECK(inode_put(dir) == FS_OK);
	CHECK(root->size == 0 && fs.nodes.len == 1);
	CHECK(inode_put(root) == FS_OK && fs_destroy(&fs) == FS_OK);
	CHECK(fs.root == NULL && fs.nodes.len == 0 && fs_destroy(&fs) == FS_EINVAL);
	return 0;
}

static int io_checks(void) {
	struct fs fs = {0};
	struct inode *file, *root;
	CHECK(fs_init(&fs, 0) == FS_OK);
	CHECK(inode_alloc(&fs, FILE_NODE, &file) == FS_OK);
	CHECK(inode_get(&fs, 1, &root) == FS_OK);
	CHECK(inode_read(root, output, 0, 1) == FS_EISDIR);
	CHECK(inode_write(root, input, 0, 1) == FS_EISDIR);
	CHECK(inode_truncate(root, 0) == FS_EISDIR);
	CHECK(inode_read(file, NULL, 0, 0) == 0);
	CHECK(inode_write(file, NULL, 8192, 0) == 0 && file->size == 0);
	CHECK(inode_read(file, NULL, 0, 1) == FS_EINVAL);
	CHECK(inode_write(file, NULL, 0, 1) == FS_EINVAL);
	for (ksize_t i = 0; i < sizeof(input); i++) input[i] = (u8_t)(i * 37 + 11);
	CHECK(inode_write(file, input, 13, 3000) == 3000 && file->size == 3013);
	CHECK(inode_read(file, output, 0, sizeof(output)) == 3013);
	CHECK(zero_bytes(output, 13) && equal_bytes(output + 13, input, 3000));
	CHECK(inode_write(file, "abcdefghijklmnopqrst", 510, 20) == 20);
	CHECK(inode_read(file, output, 509, 22) == 22);
	CHECK(output[0] == input[496] && output[21] == input[517]);
	CHECK(equal_bytes(output + 1, (const u8_t *)"abcdefghijklmnopqrst", 20));
	CHECK(inode_truncate(file, 519) == FS_OK && file->blocks.len == 2);
	CHECK(inode_truncate(file, 2000) == FS_OK);
	CHECK(inode_read(file, output, 510, 1490) == 1490);
	CHECK(equal_bytes(output, (const u8_t *)"abcdefghi", 9));
	CHECK(zero_bytes(output + 9, 1481));
	CHECK(inode_truncate(file, 512) == FS_OK && file->blocks.len == 1);
	CHECK(inode_truncate(file, 1024) == FS_OK);
	CHECK(inode_read(file, output, 512, 512) == 512 && zero_bytes(output, 512));
	CHECK(inode_truncate(file, 0) == FS_OK && file->blocks.len == 0);
	CHECK(inode_write(file, "Z", FS_MAX_FILE_SIZE - 1, 1) == 1);
	CHECK(file->blocks.len == 1 && file->size == FS_MAX_FILE_SIZE);
	CHECK(inode_read(file, output, FS_MAX_FILE_SIZE - 512, KSIZE_MAX) == 512);
	CHECK(zero_bytes(output, 511) && output[511] == 'Z');
	CHECK(inode_write(file, "x", FS_MAX_FILE_SIZE, 1) == FS_EFBIG);
	CHECK(inode_write(file, input, U64_MAX, 1) == FS_EFBIG);
	CHECK(inode_write(file, input, 0, KSIZE_MAX) == FS_EFBIG);
	CHECK(inode_write(file, NULL, FS_MAX_FILE_SIZE, 0) == 0);
	CHECK(inode_truncate(file, U64_MAX) == FS_EFBIG);
	CHECK(inode_read(file, output, U64_MAX, 1) == 0);
	CHECK(inode_read(file, output, FS_MAX_FILE_SIZE - 1, 1) == 1 && output[0] == 'Z');
	CHECK(inode_put(file) == FS_OK && inode_put(root) == FS_OK);
	CHECK(fs.nodes.len == 1 && fs_destroy(&fs) == FS_OK);
	return 0;
}

static int limits_checks(void) {
	struct fs fs = {0};
	struct inode *root, *file, *found;
	CHECK(fs_init(&fs, 0) == FS_OK && inode_get(&fs, 1, &root) == FS_OK);
	char name[FS_NAME_MAX + 2];
	for (ksize_t i = 0; i < sizeof(name) - 1; i++) name[i] = 'n';
	name[FS_NAME_MAX] = '\0';
	CHECK(inode_create(root, name, FILE_NODE, &file) == FS_OK);
	CHECK(dir_lookup(root, name, &found) == FS_OK && found == file);
	CHECK(inode_put(found) == FS_OK);
	name[FS_NAME_MAX] = 'n';
	name[FS_NAME_MAX + 1] = '\0';
	found = root;
	CHECK(dir_lookup(root, name, &found) == FS_ENAMETOOLONG && found == root);
	CHECK(inode_create(root, name, FILE_NODE, &found) == FS_ENAMETOOLONG);
	CHECK(fs_lookup(&fs, root, name, &found) == FS_ENAMETOOLONG);
	for (ksize_t i = 0; i < FS_PATH_MAX + 1; i++) input[i] = '/';
	input[FS_PATH_MAX + 1] = 0;
	CHECK(fs_lookup(&fs, root, (char *)input, &found) == FS_ENAMETOOLONG);
	input[FS_PATH_MAX] = 0;
	CHECK(fs_lookup(&fs, root, (char *)input, &found) == FS_OK && found == root);
	CHECK(inode_put(found) == FS_OK);
	file->ref = U32_MAX;
	CHECK(inode_ref(file) == FS_EOVERFLOW);
	found = root;
	CHECK(inode_get(&fs, file->inum, &found) == FS_EOVERFLOW && found == root);
	file->ref = 1;
	file->nlink = U32_MAX;
	CHECK(dir_link(root, "overflow", file) == FS_EOVERFLOW && root->size == 1);
	file->nlink = 1;
	fs.next_inum = U64_MAX;
	CHECK(inode_alloc(&fs, FILE_NODE, &found) == FS_OK && found->inum == U64_MAX);
	CHECK(inode_put(found) == FS_OK && fs.next_inum == 0);
	found = root;
	CHECK(inode_alloc(&fs, FILE_NODE, &found) == FS_EOVERFLOW && found == root);
	CHECK(inode_put(file) == FS_OK && inode_put(root) == FS_OK);
	CHECK(fs_destroy(&fs) == FS_OK);
	return 0;
}

static int reserve_pages(struct buddy_allocator *allocator, ksize_t leave, ksize_t *count) {
	*count = 0;
	while (free_pages(allocator) > leave) {
		CHECK(*count < sizeof(reserved) / sizeof(reserved[0]));
		CHECK(buddy_alloc(allocator, 0, &reserved[(*count)++]) == BUDDY_OK);
	}
	return 0;
}

static int teardown_checks(void) {
	struct fs fs = {0};
	struct inode *root, *current, *next, *file;
	CHECK(fs_init(&fs, 0) == FS_OK && inode_get(&fs, 1, &root) == FS_OK);
	CHECK(inode_ref(root) == FS_OK);
	current = root;
	/* Teardown must handle a nested tree and shared targets without recursion
	 * or following pointers to inodes that it has already freed.
	 */
	for (ksize_t depth = 0; depth < 48; depth++) {
		CHECK(inode_create(current, "child", DIR_NODE, &next) == FS_OK);
		CHECK(inode_put(current) == FS_OK);
		current = next;
	}
	CHECK(fs_getpath(&fs, current, (char *)output, sizeof(output)) == FS_OK);
	CHECK(equal_bytes(output, (const u8_t *)"/child/child", 12));
	CHECK(output[48 * 6] == 0);
	CHECK(inode_create(current, "file", FILE_NODE, &file) == FS_OK);
	CHECK(dir_link(root, "alias", file) == FS_OK);
	CHECK(inode_write(file, "retained", 1024, 8) == 8);
	CHECK(inode_put(file) == FS_OK && inode_put(current) == FS_OK);
	CHECK(inode_put(root) == FS_OK && fs.nodes.len == 50);
	CHECK(fs_destroy(&fs) == FS_OK);
	return 0;
}

static int directory_path_checks(void) {
	struct fs fs = {0};
	struct inode *current, *next, *found;
	CHECK(fs_init(&fs, 0) == FS_OK);
	CHECK(inode_get(&fs, 1, &current) == FS_OK);
	char name[FS_NAME_MAX + 1];
	for (ksize_t i = 0; i < FS_NAME_MAX; i++) name[i] = 'n';
	name[FS_NAME_MAX] = 0;
	for (ksize_t depth = 0; depth < 16; depth++) {
		CHECK(inode_create(current, name, DIR_NODE, &next) == FS_OK);
		CHECK(inode_put(current) == FS_OK);
		current = next;
	}
	/* Sixteen 63-byte names plus slashes exactly fit the path limit. */
	CHECK(fs_getpath(&fs, current, (char *)output, sizeof(output)) == FS_OK);
	CHECK(output[FS_PATH_MAX] == 0);
	CHECK(fs_lookup(&fs, NULL, (char *)output, &found) == FS_OK && found == current);
	CHECK(inode_put(found) == FS_OK);
	CHECK(fs_getpath(&fs, current, (char *)output, FS_PATH_MAX) == FS_ENAMETOOLONG);
	struct fs_dirent entry = {.inum = 123};
	CHECK(dir_read(current, 0, &entry) == 0 && entry.inum == 123);
	CHECK(inode_create(current, "too-deep", DIR_NODE, &next) == FS_OK);
	CHECK(inode_put(current) == FS_OK);
	CHECK(fs_getpath(&fs, next, (char *)output, sizeof(output)) == FS_ENAMETOOLONG);
	CHECK(inode_put(next) == FS_OK && fs_destroy(&fs) == FS_OK);
	return 0;
}

static int release_pages(struct buddy_allocator *allocator, ksize_t count) {
	while (count != 0) CHECK(buddy_free(allocator, &reserved[--count]) == BUDDY_OK);
	return 0;
}

static int exhaustion_checks(struct buddy_allocator *allocator) {
	ksize_t baseline = free_pages(allocator), count;
	struct fs fs = {0};
	int err = reserve_pages(allocator, 0, &count);
	if (err != 0) return err;
	CHECK(fs_init(&fs, 9) == FS_ENOMEM && fs.root == NULL && fs.dev == 0);
	err = release_pages(allocator, count);
	if (err != 0) return err;
	CHECK(fs_init(&fs, 0) == FS_OK);
	struct inode *root, *file, *found;
	CHECK(inode_get(&fs, 1, &root) == FS_OK);
	err = reserve_pages(allocator, 0, &count);
	if (err != 0) return err;
	/* An inode fits in the root's slab, but a first dir_entry needs a new
	 * slab. Failure must discard the inode and leave the namespace unchanged.
	 */
	found = root;
	CHECK(inode_create(root, "fail", FILE_NODE, &found) == FS_ENOMEM && found == root);
	CHECK(fs.nodes.len == 1 && root->size == 0 && root->ref == 1);
	struct inode *held[32];
	ksize_t used = 0;
	while (inode_alloc(&fs, FILE_NODE, &found) == FS_OK) {
		CHECK(used < sizeof(held) / sizeof(held[0]));
		held[used++] = found;
	}
	CHECK(used > 0);
	found = root;
	CHECK(inode_alloc(&fs, FILE_NODE, &found) == FS_ENOMEM && found == root);
	while (used != 0) CHECK(inode_put(held[--used]) == FS_OK);
	err = release_pages(allocator, count);
	if (err != 0) return err;
	CHECK(inode_put(root) == FS_OK && fs_destroy(&fs) == FS_OK);
	CHECK(free_pages(allocator) == baseline);

	int failures = 0, succeeded = 0;
	for (ksize_t available = 0; available < 8; available++) {
		CHECK(fs_init(&fs, 0) == FS_OK);
		CHECK(inode_alloc(&fs, FILE_NODE, &file) == FS_OK);
		CHECK(inode_write(file, "A", 0, 1) == 1);
		struct file handle = {0};
		CHECK(file_open(&handle, file, FILE_WRITE) == FS_OK);
		err = reserve_pages(allocator, available, &count);
		if (err != 0) return err;
		memset(input, 'B', sizeof(input));
		i64_t result = file_write(&handle, input, sizeof(input));
		if (result == FS_ENOMEM) {
			failures++;
			CHECK(file->size == 1 && file->blocks.len == 1 && handle.offset == 0);
			CHECK(inode_read(file, output, 0, 2) == 1 && output[0] == 'A');
			CHECK(free_pages(allocator) == available);
		} else {
			CHECK(result == sizeof(input) && handle.offset == sizeof(input));
			CHECK(inode_read(file, output, 0, sizeof(output)) == sizeof(output));
			CHECK(equal_bytes(input, output, sizeof(input)));
			succeeded = 1;
		}
		err = release_pages(allocator, count);
		if (err != 0) return err;
		CHECK(file_close(&handle) == FS_OK && inode_put(file) == FS_OK);
		CHECK(fs_destroy(&fs) == FS_OK && free_pages(allocator) == baseline);
		if (succeeded) break;
	}
	CHECK(failures >= 2 && succeeded);
	return 0;
}

int inode_test(struct buddy_allocator *allocator) {
	ksize_t before = free_pages(allocator);
	int err = namespace_checks();
	if (err == 0) err = io_checks();
	if (err == 0) err = limits_checks();
	if (err == 0) err = teardown_checks();
	if (err == 0) err = directory_path_checks();
	if (err == 0) err = exhaustion_checks(allocator);
	if (err != 0) return err;
	CHECK(free_pages(allocator) == before);
	/* Empty heap also proves no live slab objects remain after teardown. */
	CHECK(kalloc_init(allocator) == KALLOC_OK);
	return 0;
}
