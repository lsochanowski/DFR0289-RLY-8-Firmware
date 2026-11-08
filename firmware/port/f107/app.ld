/* RLY-8 application image (v2+): runs behind the bootloader.
 * Flash map: see core/rly_ota.h. The .bin produced from this image is the OTA update file. */
ENTRY(Reset_Handler)

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08002000, LENGTH = 120K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    .isr_vector : {
        KEEP(*(.isr_vector))
    } > FLASH

    /* Image header at a fixed offset (RLY_APP_INFO_OFFSET) so the web panel and the
     * bootloader can validate an uploaded file before trusting it. */
    .app_info ORIGIN(FLASH) + 0x200 : {
        KEEP(*(.app_info))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    _sidata = LOADADDR(.data);

    .data : {
        _sdata = .;
        *(.data*)
        _edata = .;
    } > RAM AT > FLASH

    /* One past the last byte of the .bin (= link address + file size). */
    _image_end = LOADADDR(.data) + SIZEOF(.data);

    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        _ebss = .;
    } > RAM

    end = .;
    _end = .;
    __bss_start__ = _sbss;
    __bss_end__ = _ebss;
}

ASSERT(ADDR(.app_info) == ORIGIN(FLASH) + 0x200, "app_info must sit at offset 0x200")
ASSERT(SIZEOF(.isr_vector) <= 0x200, "vector table overlaps app_info")
