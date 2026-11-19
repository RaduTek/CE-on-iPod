;
; s5l8702.inc - S5L8702 physical register addresses for assembly (startup.s)
;
; Keep in sync with s5l8702.h. startup.s runs with the MMU off, so these are
; physical addresses.
;

; Memory
S5L_DRAM_PA_BASE        EQU     0x08000000
S5L_IRAM0_PA_BASE       EQU     0x22000000
; Early stack: top of the first 32 KB of IRAM0 (unused by CE)
S5L_EARLY_STACK_TOP     EQU     0x22008000

; Clock gates
S5L_PWRCON1_PA          EQU     0x3C50004C
S5L_PWRCON1_UART        EQU     0x00000200

; GPIO
S5L_PCON0_PA            EQU     0x3CF00000
S5L_PCON0_UART0_MASK    EQU     0x00FF0000
S5L_PCON0_UART0_FUNC    EQU     0x00220000

; VICs
S5L_VIC0_PA             EQU     0x38E00000
S5L_VIC1_PA             EQU     0x38E01000
VIC_INTENCLEAR          EQU     0x014
VIC_ADDRESS             EQU     0xF00

; Timers
S5L_TIMER_PA            EQU     0x3C700000
TIMER_TBCON             EQU     0x20
TIMER_TBCMD             EQU     0x24

; UART0 (dock connector)
S5L_UART0_PA            EQU     0x3CC00000
UART_ULCON              EQU     0x00
UART_UCON               EQU     0x04
UART_UFCON              EQU     0x08
UART_UMCON              EQU     0x0C
UART_UTRSTAT            EQU     0x10
UART_UTXH               EQU     0x20
UART_UBRDIV             EQU     0x28
UART_UBRCONTX           EQU     0x34
UART_UBRCONRX           EQU     0x38

; CP15 control register bits
CP15_CTRL_M             EQU     0x00000001      ; MMU
CP15_CTRL_C             EQU     0x00000004      ; D-cache
CP15_CTRL_I             EQU     0x00001000      ; I-cache
CP15_CTRL_V             EQU     0x00002000      ; high vectors

        END
