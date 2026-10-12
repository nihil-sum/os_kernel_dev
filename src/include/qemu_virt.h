#ifndef QEMU_VIRT_H
#define QEMU_VIRT_H

#include "ktypes.h"

/* Match `qemu-system-riscv64 -machine virt -m 128M -bios none`. */
#define QEMU_RAM_BASE U64_C(0x80000000)
#define QEMU_RAM_SIZE (U64_C(128) * 1024 * 1024)
#define QEMU_RAM_END (QEMU_RAM_BASE + QEMU_RAM_SIZE)
#define QEMU_UART_BASE U64_C(0x10000000)
#define QEMU_TEST_BASE U64_C(0x00100000)

#endif
