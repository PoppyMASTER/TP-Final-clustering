/*
 * Script de linker pour le mini OS guest
 * Place le code exactement à 0x100000 (1 Mo)
 * — c'est là que MiniHV le charge en mémoire guest
 */
ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text : {
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }
}
