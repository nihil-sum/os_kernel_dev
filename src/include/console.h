#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>
#include <stdint.h>

/* Polled QEMU virt NS16550 UART. Single hart; no interrupts required. */
void console_putc(char ch);
void console_puts(const char *text);
void console_u64(uint64_t value);
void console_i64(int64_t value);
void console_hex(uint64_t value);
/* 0: line read, -1: overlong line discarded, 1: Ctrl-D on an empty line.
 * Includes echo, CR/LF, backspace, Ctrl-U, and Ctrl-C. size includes NUL.
 */
int console_readline(char *line, size_t size);
_Noreturn void console_shutdown(int failed);
#endif
