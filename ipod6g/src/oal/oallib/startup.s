;------------------------------------------------------------------------------
;
;   File:  startup.s
;
;   Hardware startup for the iPod classic (S5L8702, ARM926EJ-S).
;
;   Entry state, from the Rockbox bootloader (bootloader/ipod-s5l87xx.c):
;     - the image (nk.nb0) is at PA 0x08000000 and we were branched to
;       through romimage's jump page at its start
;     - CPU in SYS mode, IRQ and FIQ masked
;     - MMU, I-cache and D-cache ON with an identity (VA == PA) map
;     - VIC sources still enabled, Timer B running the Rockbox tick
;     - UART clock gated
;
;   Before KernelStart we must leave a clean "reset-like" state: SVC mode,
;   interrupts masked at the VICs, D-cache written back, MMU and caches off.
;   The map is identity, so turning the MMU off does not move the PC.
;
;------------------------------------------------------------------------------

        OPT     2                               ; disable listing
        INCLUDE kxarm.h
        INCLUDE s5l8702.inc
        OPT     1                               ; reenable listing

        IMPORT  KernelStart

        STARTUPTEXT

;------------------------------------------------------------------------------
;
;   StartUp: main entry point
;
;------------------------------------------------------------------------------

        LEAF_ENTRY StartUp

        ;----------------------------------------------------------------------
        ; 1. SVC mode, IRQ and FIQ masked
        ;----------------------------------------------------------------------
        mov     r0, #0xD3                       ; SVC mode (0x13) | I | F
        msr     cpsr_c, r0

        ;----------------------------------------------------------------------
        ; 2. Mask every VIC source, finish any in-service IRQ and stop the
        ;    Rockbox tick (Timer B)
        ;----------------------------------------------------------------------
        mvn     r0, #0
        ldr     r1, =S5L_VIC0_PA
        str     r0, [r1, #VIC_INTENCLEAR]
        ldr     r2, =S5L_VIC1_PA
        str     r0, [r2, #VIC_INTENCLEAR]
        mov     r0, #0
        str     r0, [r1, #VIC_ADDRESS]
        str     r0, [r2, #VIC_ADDRESS]

        ldr     r1, =S5L_TIMER_PA
        str     r0, [r1, #TIMER_TBCMD]          ; TB_EN = 0: stop timer B
        ldr     r0, [r1, #TIMER_TBCON]
        str     r0, [r1, #TIMER_TBCON]          ; write back clears pending status

        ;----------------------------------------------------------------------
        ; 3. Write back and invalidate the D-cache (ARM926 test, clean and
        ;    invalidate), drain the write buffer, then turn off MMU and caches
        ;    (as Rockbox crt0.S does), and invalidate caches and TLBs.
        ;----------------------------------------------------------------------
CleanDCache
        mrc     p15, 0, r15, c7, c14, 3         ; test, clean and invalidate
        bne     CleanDCache
        mov     r0, #0
        mcr     p15, 0, r0, c7, c10, 4          ; drain write buffer

        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #CP15_CTRL_I
        bic     r0, r0, #CP15_CTRL_C
        bic     r0, r0, #CP15_CTRL_M
        bic     r0, r0, #CP15_CTRL_V            ; low vectors; the kernel sets its own
        mcr     p15, 0, r0, c1, c0, 0
        nop
        nop
        nop

        mov     r0, #0
        mcr     p15, 0, r0, c7, c7, 0           ; invalidate I and D caches
        mcr     p15, 0, r0, c8, c7, 0           ; invalidate I and D TLBs
        mcr     p15, 0, r0, c7, c10, 4          ; drain write buffer

        ;----------------------------------------------------------------------
        ; 4. Early stack in IRAM0 (physical, MMU off)
        ;----------------------------------------------------------------------
        ldr     sp, =S5L_EARLY_STACK_TOP

        ;----------------------------------------------------------------------
        ; 5. Optional early UART output on the dock connector.
        ;    EARLY_UART is TRUE for IMGNOKITL=1 builds or BSP_EARLYUART=1
        ;    (see oallib\sources). In KITL builds the raw text would arrive
        ;    ahead of the KITL stream, so it is off by default there.
        ;----------------------------------------------------------------------
        IF EARLY_UART
        bl      EarlyUartInit
        mov     r0, #0x0D
        bl      EarlyPutc
        mov     r0, #0x0A
        bl      EarlyPutc
        mov     r0, #'C'
        bl      EarlyPutc
        mov     r0, #'E'
        bl      EarlyPutc
        mov     r0, #'>'
        bl      EarlyPutc
        mov     r0, #0x0D
        bl      EarlyPutc
        mov     r0, #0x0A
        bl      EarlyPutc
        ENDIF

        ;----------------------------------------------------------------------
        ; 6. Jump to the kernel with the physical address of OEMAddressTable
        ;    in r0 (we run at our physical address, so adr gives the PA).
        ;----------------------------------------------------------------------
        adr     r0, g_oalAddressTable
        b       KernelStart

        LTORG

        ;
        ; OEMAddressTable (keep it inside StartUp's area, close to the adr)
        ;
        INCLUDE OEMAddrTable.inc

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyUartInit: UART0 115200 8N1 from ECLK (12 MHz), physical addresses.
;   Same sequence as S5lUart0Init in s5l8702_uart.h. Uses r0-r2.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyUartInit

        ; ungate the UART clock: PWRCON1 &= ~(1 << 9)
        ldr     r1, =S5L_PWRCON1_PA
        ldr     r0, [r1]
        bic     r0, r0, #S5L_PWRCON1_UART
        str     r0, [r1]

        ; GPIO 0.4/0.5 -> UART0 TXD/RXD
        ldr     r1, =S5L_PCON0_PA
        ldr     r0, [r1]
        bic     r0, r0, #S5L_PCON0_UART0_MASK
        orr     r0, r0, #S5L_PCON0_UART0_FUNC
        str     r0, [r1]

        ldr     r1, =S5L_UART0_PA

        ; reset the port
        mov     r0, #0
        str     r0, [r1, #UART_UCON]
        str     r0, [r1, #UART_ULCON]
        str     r0, [r1, #UART_UMCON]
        mvn     r0, #0
        str     r0, [r1, #UART_UTRSTAT]

        ; FIFOs on and reset, nRTS active
        mov     r0, #0x07
        str     r0, [r1, #UART_UFCON]
        mov     r0, #0x01
        str     r0, [r1, #UART_UMCON]

        ; clock = ECLK, 8N1, 115200 (UBRDIV 6, fine-tune 0xFFFFFF)
        mov     r0, #0x400
        str     r0, [r1, #UART_UCON]
        mov     r0, #0x03
        str     r0, [r1, #UART_ULCON]
        mov     r0, #6
        str     r0, [r1, #UART_UBRDIV]
        ldr     r0, =0x00FFFFFF
        str     r0, [r1, #UART_UBRCONRX]
        str     r0, [r1, #UART_UBRCONTX]

        ; enable Rx and Tx in polling mode
        ldr     r0, =0x00000405
        str     r0, [r1, #UART_UCON]

        mov     pc, lr

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyPutc: write the byte in r0 to UART0 (polled). Uses r1-r2.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyPutc

        ldr     r1, =S5L_UART0_PA
EarlyPutcWait
        ldr     r2, [r1, #UART_UTRSTAT]
        tst     r2, #0x02                       ; Tx buffer empty?
        beq     EarlyPutcWait
        str     r0, [r1, #UART_UTXH]

        mov     pc, lr

        ENTRY_END

        END
