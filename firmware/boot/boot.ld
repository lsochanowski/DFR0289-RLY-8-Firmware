/* RLY-8 bootloader: first 8 KB of flash. Uses only the top of RAM for its stack. */
ENTRY(Boot_Reset)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 8K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
    .text : { *(.text*) *(.rodata*) } > FLASH
    _sidata = LOADADDR(.data);
    .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH
    .bss : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
}
