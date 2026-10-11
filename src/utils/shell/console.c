#include "console.h"
#include "qemu_virt.h"

#define UART_LSR 5u
#define UART_RX_READY 0x01u
#define UART_TX_READY 0x20u

static volatile unsigned char *const uart = (void *)(uintptr_t)QEMU_UART_BASE;

static void raw_putc(char ch) {
        while ((uart[UART_LSR] & UART_TX_READY) == 0) {}
        uart[0] = (unsigned char)ch;
}

void console_putc(char ch) {
        if (ch == '\n') raw_putc('\r');
        raw_putc(ch);
}

void console_puts(const char *text) {
        while (*text != '\0') console_putc(*text++);
}

void console_u64(uint64_t value) {
        char digits[20];
        size_t count = 0;
        do {
                digits[count++] = (char)('0' + value % 10);
                value /= 10;
        } while (value != 0);
        while (count != 0) console_putc(digits[--count]);
}

void console_i64(int64_t value) {
        if (value < 0) {
                console_putc('-');
                console_u64((uint64_t)(-(value + 1)) + 1);
        } else console_u64((uint64_t)value);
}

void console_hex(uint64_t value) {
        const char *digits = "0123456789abcdef";
        console_puts("0x");
        int shift = 60;
        while (shift > 0 && ((value >> shift) & 15) == 0) shift -= 4;
        for (; shift >= 0; shift -= 4) console_putc(digits[(value >> shift) & 15]);
}

static unsigned char getc_blocking(void) {
        while ((uart[UART_LSR] & UART_RX_READY) == 0) {}
        return uart[0];
}

int console_readline(char *line, size_t size) {
        static int skip_lf;
        if (line == NULL || size == 0) return -1;
        size_t count = 0;
        int overflow = 0;
        for (;;) {
                unsigned char ch = getc_blocking();
                if (skip_lf) {
                        skip_lf = 0;
                        if (ch == '\n') continue;
                }
                if (ch == '\r' || ch == '\n') {
                        skip_lf = ch == '\r';
                        line[count] = '\0';
                        console_putc('\n');
                        return overflow ? -1 : 0;
                }
                if (ch == 4 && count == 0 && !overflow) return 1;
                if (ch == 3) {
                        console_puts("^C\n");
                        line[0] = '\0';
                        return 0;
                }
                if (ch == 21) {
                        while (count != 0) { console_puts("\b \b"); count--; }
                        overflow = 0;
                        continue;
                }
                if (ch == 8 || ch == 127) {
                        if (count != 0 && !overflow) {
                                count--;
                                console_puts("\b \b");
                        }
                        continue;
                }
                if ((ch < 32 && ch != '\t') || ch > 126) continue;
                if (count + 1 < size && !overflow) {
                        line[count++] = (char)ch;
                        console_putc((char)ch);
                } else overflow = 1;
        }
}

_Noreturn void console_shutdown(int failed) {
        volatile unsigned int *test = (void *)(uintptr_t)QEMU_TEST_BASE;
        *test = failed ? 0x3333u : 0x5555u;
        for (;;) __asm__ volatile("wfi");
}
