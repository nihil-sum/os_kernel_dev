#ifndef CONSOLE_H
#define CONSOLE_H

#include "ktypes.h"

/* Polled QEMU virt NS16550 UART. Single hart; no interrupts required. */
void console_putc(char ch);
void console_puts(const char *text);
void console_u64(u64_t value);
void console_i64(i64_t value);
void console_hex(u64_t value);
/* 0: line read, -1: overlong line discarded, 1: Ctrl-D on an empty line,
 * 2: Ctrl-C canceled input (line is empty).
 * Includes echo, CR/LF, backspace, Ctrl-U, and Ctrl-C. size includes NUL.
 */
int console_readline(char *line, ksize_t size);
_Noreturn void console_shutdown(int failed);
#endif
