/* SPDX-License-Identifier: MIT */
OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
  . = 0;
  .text : { KEEP(*(.text.entry)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) *(.got .got.*) }
  .bss : { *(.bss .bss.* COMMON) }
  ASSERT(SIZEOF(.data) == 0, "Shim must not contain absolute data/GOT pointers")
  ASSERT(SIZEOF(.bss) == 0, "Shim must not depend on a BSS initialization routine")
  .stack : {
    . = ALIGN(16);
    __stack_bottom = .;
    . += 0x4000;
    __stack_top = .;
    BYTE(0)
    . = ALIGN(4096);
  }
  _payload = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
