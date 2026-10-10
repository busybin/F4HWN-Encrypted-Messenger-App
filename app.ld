/* Overlay-app link script (POC).
 *
 * The blob is linked to run at the PY25Q16 sector-cache overlay VMA (shared with
 * the multiboot RAM stub — never both active at once). Code + rodata + data +
 * bss must all fit in the resident 8 KiB overlay. The loader clears the complete
 * overlay before copying, so .bss is NOLOAD and is not emitted into the binary.
 * The slimmed receipt/ID build fits code+state in 8 KiB.
 *
 * VMA must match ADDR(.mb_workspace) in Core/py32f071xb.ld. If the firmware RAM
 * layout shifts, update APP_VMA and the loader's compile-time assert catches a
 * mismatch.
 *
 * Built for the 8 KiB-overlay custom Labs firmware (APP_OVERLAY_MAX=0x2000).
 * Requires the custom firmware; use the patched local UVStudio to install. */

/* APP_VMA is the target firmware's __mb_workspace_start (read from its .map). It
 * varies with the RAM layout, so pass it from the build: -Wl,--defsym,APP_VMA=0x...
 * The default is only a fallback for a standalone size check. */
APP_VMA    = DEFINED(APP_VMA) ? APP_VMA : 0x20000280;
APP_LENGTH = 0x2000;       /* 8 KiB sector-cache overlay (custom firmware)     */
APP_CODE_MAX = 0x2000;     /* emitted code/rodata/data budget: 7 KiB           */

ENTRY(app_main)

MEMORY {
    APP (rwx) : ORIGIN = APP_VMA, LENGTH = APP_LENGTH
}

SECTIONS {
    .app APP_VMA : {
        KEEP(*(.text.entry))      /* app_main pinned to offset 0 */
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(4);
        *(.data .data.*)
        . = ALIGN(4);
    } > APP

    .bss (NOLOAD) : {
        __app_bss_start = .;
        *(.bss .bss.* COMMON)
        . = ALIGN(4);
        __app_bss_end = .;
    } > APP

    __app_end = .;
    __app_image_end = LOADADDR(.app) + SIZEOF(.app);
    ASSERT(__app_end <= APP_VMA + APP_LENGTH,
           "SECMSG overlay app overflows the 8 KiB overlay")
    ASSERT(__app_image_end <= APP_VMA + APP_CODE_MAX,
           "SECMSG app image exceeds the 8 KiB code budget")

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.eh_frame*) *(.comment) *(.note.*) }
}
