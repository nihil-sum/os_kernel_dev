OUTPUT_ARCH(riscv)
ENTRY(_start)

PHDRS
{
	text PT_LOAD FLAGS(5);
	data PT_LOAD FLAGS(6);
}

SECTIONS
{
	. = 0x80000000;
	.text : {
		KEEP(*(.text.init))
		*(.text .text.*)
	} :text
	.rodata : { *(.rodata .rodata.*) } :text
	. = ALIGN(4096);
	.data : { *(.data .data.*) } :data
	. = ALIGN(8);
	__bss_start = .;
	.bss : {
		*(.bss .bss.* COMMON)
		. = ALIGN(16);
		. += 16384;
		__stack_top = .;
	} :data
	__bss_end = .;
	. = ALIGN(4096);
	__kernel_end = .;
}
