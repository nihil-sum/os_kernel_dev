	.section .text.init, "ax"
	.globl _start
	.type _start, @function
_start:
	/* QEMU virt with -bios none enters on hart 0 in machine mode. */
	la sp, __stack_top

	/* Clear BSS before the C entry point uses static metadata. */
	la t0, __bss_start
	la t1, __bss_end
1:
	bgeu t0, t1, 2f
	sd zero, 0(t0)
	addi t0, t0, 8
	j 1b
2:
	call kmain
3:
	wfi
	j 3b
	.size _start, . - _start
