SECTIONS
{
  .branch 0x02002070 : { KEEP(*(.text.branch)) }
  .text 0x02008000 : { KEEP(*(.text.entry)) *(.text*) *(.rodata*) }
  .data : { *(.data*) }
  .bss (NOLOAD) : { *(.bss*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) *(.ARM.extab*) }
}
ASSERT(SIZEOF(.branch) == 4, "checker branch must replace exactly four bytes")
ASSERT(ADDR(.text) + SIZEOF(.text) <= 0x02010000, "payload overlaps application mapping")
ASSERT(SIZEOF(.data) == 0, "boot payload cannot require data initialization")
ASSERT(SIZEOF(.bss) == 0, "boot payload cannot require BSS initialization")
