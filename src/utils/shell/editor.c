#include "shell_fs.h"
#include "console.h"
#include "proc_fs.h"
#include "kalloc.h"
#include "kstring.h"

#define EDITOR_CAPACITY (16u * 1024u)
#define EDITOR_LINE_SIZE 256u

struct editor {
        struct fs *fs;
        struct inode *inode;  /* Existing file, retained until editor closes. */
        struct inode *parent; /* Missing file: publish its name only on save. */
        char name[FS_NAME_MAX + 1];
        char *text;
        ksize_t size;
        int dirty;
};

static int equal(const char *a, const char *b) {
        while (*a != '\0' && *a == *b) { a++; b++; }
        return *a == *b;
}

static ksize_t length(const char *text) {
        ksize_t size = 0;
        while (text[size] != '\0') size++;
        return size;
}

static void help(void) {
        console_puts("p        print numbered lines\n"
                     "a        append lines; '.' ends input, '..' inserts '.'\n"
                     "i N      insert lines before line N (N may be last+1)\n"
                     "r N      replace line N with the next input line\n"
                     "d N      delete line N\n"
                     "w / wq   save / save and quit\n"
                     "q / q!   quit if saved / discard unsaved changes\n"
                     "help     show editor commands\n"
                     "Ctrl-C cancels current input mode; Ctrl-D ends input or requests quit.\n");
}

static ksize_t lines(const struct editor *editor) {
        ksize_t count = 0;
        for (ksize_t i = 0; i < editor->size; i++)
                if (editor->text[i] == '\n') count++;
        if (editor->size != 0 && editor->text[editor->size - 1] != '\n') count++;
        return count;
}

/* end includes the newline, if present. An unterminated final line is valid. */
static int bounds(const struct editor *editor, ksize_t line,
                   ksize_t *begin, ksize_t *end) {
        if (line == 0) return PROC_EINVAL;
        ksize_t pos = 0, current = 1;
        while (pos < editor->size) {
                ksize_t start = pos;
                while (pos < editor->size && editor->text[pos] != '\n') pos++;
                if (pos < editor->size) pos++;
                if (current++ == line) { *begin = start; *end = pos; return 0; }
        }
        return PROC_EINVAL;
}

static void print(const struct editor *editor) {
        if (editor->size == 0) { console_puts("(empty)\n"); return; }
        int start = 1;
        ksize_t line = 1;
        for (ksize_t i = 0; i < editor->size; i++) {
                if (start) { console_u64(line++); console_puts(" | "); start = 0; }
                console_putc(editor->text[i]);
                if (editor->text[i] == '\n') start = 1;
        }
        if (!start) console_putc('\n');
}

static void erase(struct editor *editor, ksize_t begin, ksize_t end) {
        ksize_t count = end - begin;
        for (ksize_t i = end; i < editor->size; i++)
                editor->text[i - count] = editor->text[i];
        editor->size -= count;
        editor->dirty = 1;
}

/* Capacity is checked before modifying the buffer. Move toward the end in
 * reverse order; kernel memcpy deliberately does not promise overlap safety.
 */
static int insert(struct editor *editor, ksize_t *position, const char *text) {
        ksize_t count = length(text);
        int separator = *position == editor->size && *position != 0 &&
                        editor->text[*position - 1] != '\n';
        ksize_t added = count + 1 + (unsigned int)separator;
        if (added > EDITOR_CAPACITY - editor->size) return PROC_EFBIG;
        for (ksize_t i = editor->size; i > *position; i--)
                editor->text[i - 1 + added] = editor->text[i - 1];
        if (separator) editor->text[(*position)++] = '\n';
        memcpy(editor->text + *position, text, count);
        *position += count;
        editor->text[(*position)++] = '\n';
        editor->size += added;
        editor->dirty = 1;
        return 0;
}

static void input_error(int status) {
        if (status < 0) console_puts("editor: line too long (maximum 255 characters)\n");
}

static void insert_lines(struct editor *editor, ksize_t position) {
        console_puts("Enter text; a single '.' ends input.\n");
        for (;;) {
                char line[EDITOR_LINE_SIZE];
                console_puts("text> ");
                int status = console_readline(line, sizeof(line));
                if (status == 1) { console_putc('\n'); return; }
                if (status == 2) return;
                if (status < 0) { input_error(status); continue; }
                if (equal(line, ".")) return;
                const char *text = equal(line, "..") ? "." : line;
                if (insert(editor, &position, text) != 0) {
                        console_puts("editor: buffer full (maximum 16384 bytes)\n");
                        return;
                }
        }
}

static void replace_line(struct editor *editor, ksize_t begin, ksize_t end) {
        char text[EDITOR_LINE_SIZE];
        console_puts("text> ");
        int status = console_readline(text, sizeof(text));
        if (status != 0) {
                if (status == 1) console_putc('\n');
                input_error(status);
                return;
        }
        /* Reserve enough capacity before erasing the old line. */
        if (length(text) + 1 > EDITOR_CAPACITY - (editor->size - (end - begin))) {
                console_puts("editor: buffer full (maximum 16384 bytes)\n");
                return;
        }
        erase(editor, begin, end);
        (void)insert(editor, &begin, text);
}

static int save(struct editor *editor) {
        struct inode *target = editor->inode;
        int created = target == NULL;
        int err;
        if (created) {
                err = inode_alloc(editor->fs, FILE_NODE, &target);
                if (err != FS_OK) return proc_fs_error(err);
        }
        /* One inode_write reserves every needed block before touching live
         * bytes. Truncate only after success; shrinking requires no allocation.
         * New files remain unnamed until both content and directory entry work.
         */
        i64_t written = inode_write(target, editor->text, 0, editor->size);
        if (written < 0) err = (int)written;
        else {
                err = inode_truncate(target, editor->size);
                if (err == FS_OK && created) err = dir_link(editor->parent, editor->name, target);
        }
        if (err != FS_OK) {
                if (created) (void)inode_put(target);
                return proc_fs_error(err);
        }
        if (created) {
                editor->inode = target;
                (void)inode_put(editor->parent);
                editor->parent = NULL;
        }
        editor->dirty = 0;
        console_puts("saved bytes="); console_u64(editor->size); console_putc('\n');
        return 0;
}

static int line_number(const char *text, ksize_t *result) {
        if (*text == '\0') return PROC_EINVAL;
        ksize_t value = 0;
        for (; *text != '\0'; text++) {
                if (*text < '0' || *text > '9') return PROC_EINVAL;
                unsigned int digit = (unsigned int)(*text - '0');
                if (value > (EDITOR_CAPACITY + 1 - digit) / 10) return PROC_EINVAL;
                value = value * 10 + digit;
        }
        if (value == 0) return PROC_EINVAL;
        *result = value;
        return 0;
}

static void run(struct editor *editor) {
        help();
        for (;;) {
                char command[EDITOR_LINE_SIZE];
                console_puts("edit> ");
                int status = console_readline(command, sizeof(command));
                if (status < 0) { input_error(status); continue; }
                if (status == 2) continue;
                char *start = command;
                if (status == 1) {
                        console_putc('\n');
                        command[0] = 'q'; command[1] = '\0';
                }
                while (*start == ' ' || *start == '\t') start++;
                ksize_t count = length(start);
                while (count != 0 && (start[count - 1] == ' ' || start[count - 1] == '\t'))
                        start[--count] = '\0';
                if (equal(start, "q!")) return;
                if (equal(start, "q")) {
                        if (!editor->dirty) return;
                        console_puts("unsaved changes: use w to save or q! to discard\n");
                } else if (equal(start, "p")) print(editor);
                else if (equal(start, "a")) insert_lines(editor, editor->size);
                else if (equal(start, "help")) help();
                else if (equal(start, "w") || equal(start, "wq")) {
                        int err = save(editor);
                        if (err != 0) {
                                console_puts("save failed: error "); console_i64(err);
                                console_puts("; changes kept in editor\n");
                        } else if (equal(start, "wq")) return;
                } else if ((start[0] == 'i' || start[0] == 'r' || start[0] == 'd') &&
                           (start[1] == ' ' || start[1] == '\t')) {
                        char *arg = start + 2;
                        while (*arg == ' ' || *arg == '\t') arg++;
                        ksize_t line, begin, end;
                        if (line_number(arg, &line) != 0) {
                                console_puts("editor: invalid line number\n");
                                continue;
                        }
                        if (start[0] == 'i' && line == lines(editor) + 1) begin = editor->size;
                        else if (bounds(editor, line, &begin, &end) != 0) {
                                console_puts("editor: no such line\n");
                                continue;
                        }
                        if (start[0] == 'i') insert_lines(editor, begin);
                        else if (start[0] == 'r') replace_line(editor, begin, end);
                        else erase(editor, begin, end);
                } else if (count != 0) console_puts("editor: unknown command; use help\n");
        }
}

int shell_editor(struct fs *fs, struct inode *cwd, const char *path) {
        struct editor editor = {.fs = fs};
        int err = fs_lookup(fs, cwd, path, &editor.inode);
        if (err == FS_ENOENT) {
                int trailing;
                err = shell_path_parent(fs, cwd, path, &editor.parent, editor.name, &trailing);
                if (err == FS_OK && trailing) err = FS_ENOTDIR;
        }
        if (err != FS_OK) goto out;
        if (editor.inode != NULL) {
                if (editor.inode->type != FILE_NODE) { err = FS_EISDIR; goto out; }
                if (editor.inode->size > EDITOR_CAPACITY) {
                        console_puts("editor limit is 16384 bytes\n");
                        err = FS_EFBIG;
                        goto out;
                }
                editor.size = editor.inode->size;
        }
        editor.text = kalloc(EDITOR_CAPACITY);
        if (editor.text == NULL) { err = FS_ENOMEM; goto out; }
        if (editor.inode != NULL) {
                i64_t count = inode_read(editor.inode, editor.text, 0, editor.size);
                if (count < 0) { err = (int)count; goto out; }
                for (ksize_t i = 0; i < editor.size; i++) {
                        unsigned char ch = (unsigned char)editor.text[i];
                        if (ch != '\n' && ch != '\t' && (ch < 32 || ch > 126)) {
                                console_puts("editor supports ASCII text with LF newlines; file unchanged\n");
                                err = FS_EINVAL;
                                goto out;
                        }
                }
        }
        console_puts("Editing "); console_puts(path);
        console_puts(editor.inode == NULL ? " (new file)\n" : "\n");
        console_puts("bytes="); console_u64(editor.size);
        console_puts(" lines="); console_u64(lines(&editor)); console_putc('\n');
        run(&editor);
out:
        if (editor.inode != NULL) (void)inode_put(editor.inode);
        if (editor.parent != NULL) (void)inode_put(editor.parent);
        (void)kfree(editor.text);
        return proc_fs_error(err);
}
