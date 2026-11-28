;
; s5l8720.inc - S5L8720 physical addresses for assembly (startup.s)
;
; Keep in sync with s5l8720.h and config.bib. startup.s runs with the MMU
; off, so these are physical addresses. Values from openiBoot, unverified.
;

; Memory
S5L_DRAM_PA_BASE        EQU     0x08000000      ; NK start = link address of the image
S5L_DRAM_PA_END         EQU     0x10000000
S5L_FB_RESERVED_PA      EQU     0x0FC00000      ; top 4 MB, not CE RAM (config.bib)
S5L_IMAGE_VA_BASE       EQU     0x80000000      ; ROMSTART in config.bib
S5L_NK_MAXLEN           EQU     0x02000000      ; NKLEN in config.bib

; Scratch memory before the kernel runs (inside CE RAM, free again later)
S5L_EARLY_STACK_TOP     EQU     0x0FBE0000
S5L_RELOC_STUB_PA       EQU     0x0FBF0000      ; copy of the relocation stub
S5L_EARLY_FLAGS_PA      EQU     0x0FBF0F00      ; word: nonzero = UART does not answer
S5L_PACKED_PA           EQU     0x0E000000      ; packed image copied here before unpacking
S5L_PACKED_MAXLEN       EQU     0x01000000      ; 16 MiB (iBSS upload buffer)

; Pack header in the jump page (files\mkimg3.ps1 -Compress)
PACK_MAGIC              EQU     0x45435A4C      ; "LZCE"
PACK_HDR_MAGIC          EQU     0x20
PACK_HDR_PACKEDLEN      EQU     0x24
PACK_HDR_HEADLEN        EQU     0x28
PACK_HDR_IMAGELEN       EQU     0x2C

; Chip ID
S5L_CHIPINFO_PA         EQU     0x3D100008

; Clock controller
S5L_CLK_PA              EQU     0x3C500000

; VICs
S5L_VIC0_PA             EQU     0x38E00000
S5L_VIC1_PA             EQU     0x38E01000
VIC_INTENABLE           EQU     0x010
VIC_INTENCLEAR          EQU     0x014
VIC_ADDRESS             EQU     0xF00

; Timers
S5L_TIMER_PA            EQU     0x3C700000
TIMER_CMD               EQU     0x04
TIMER_TICKSHIGH         EQU     0x80
TIMER_TICKSLOW          EQU     0x84
TIMER_IRQLATCH          EQU     0x118
S5L_TIMERIRQSTAT_PA     EQU     0x3C710000

; UART0 (dock connector, presumably)
S5L_UART0_PA            EQU     0x3CC00000
UART_ULCON              EQU     0x00
UART_UCON               EQU     0x04
UART_UFCON              EQU     0x08
UART_UMCON              EQU     0x0C
UART_UTRSTAT            EQU     0x10
UART_UTXH               EQU     0x20
UART_UBAUD              EQU     0x28
UTRSTAT_TX_EMPTY        EQU     0x04
UCON_MODE_MASK          EQU     0x0F
UCON_MODE_POLL          EQU     0x05

; Display controller, window 0
S5L_CLCD_PA             EQU     0x38900000
CLCD_W0_FORMAT          EQU     0x20
CLCD_W0_ADDR            EQU     0x24
CLCD_W0_STRIDE          EQU     0x28
CLCD_W0_SIZE            EQU     0x30

; Memory controller remap register
S5L_MIU_REMAP_PA        EQU     0x3E00000C

; Watchdog
S5L_WDTCON_PA           EQU     0x3C800000

; CP15 control register bits
CP15_CTRL_M             EQU     0x00000001      ; MMU
CP15_CTRL_C             EQU     0x00000004      ; D-cache
CP15_CTRL_Z             EQU     0x00000800      ; branch prediction
CP15_CTRL_I             EQU     0x00001000      ; I-cache
CP15_CTRL_V             EQU     0x00002000      ; high vectors

; ROM image header (romimage): 'CECE' at offset 0x40, then the ROMHDR VA
ROM_SIGNATURE_OFFSET    EQU     0x40
ROM_SIGNATURE           EQU     0x43454345
ROMHDR_PHYSLAST         EQU     12

        END
