;------------------------------------------------------------------------------
;
;   File:  startup.s
;
;   Hardware startup for the iPod touch 2G (S5L8720, ARM1176JZF-S).
;
;   Entry: the patched iBoot started the IMG3 payload (nk.nb0, trimmed) at
;   its first byte, romimage's branch to StartUp. Nothing else is known yet:
;   the load address, CPU mode, MMU and cache state are what the probe below
;   prints. Assumptions (openiBoot works the same way): any MMU mapping
;   iBoot left is an identity map, and DRAM is at PA 0x08000000-0x0FFFFFFF.
;
;   Steps:
;     1. note the hand-over state, SVC mode, IRQ/FIQ masked
;     2. mask the VICs, stop the timers iBoot left running
;     3. clean the D-cache, MMU, caches and branch prediction off
;     4. screen blue (SCREEN_MARKS), probe on the UART (EARLY_UART)
;     5. copy the image to its link address PA 0x08000000 if it runs
;        anywhere else (romimage links it at VA 0x80000000 = that PA)
;     6. screen green, KernelStart with the PA of OEMAddressTable
;
;   Runs with the MMU off from step 3 on. Before step 5 it runs at the load
;   address, so everything is position independent: branches are relative,
;   and addresses of labels are taken with RUNADR (link VA - image VA base
;   + image run address in r10).
;
;------------------------------------------------------------------------------

        OPT     2                               ; disable listing
        INCLUDE kxarm.h
        INCLUDE s5l8720.inc
        OPT     1                               ; reenable listing

        IMPORT  KernelStart

; Loops of the Tx-empty poll before the UART is declared dead
EARLY_UART_TIMEOUT      EQU     0x00010000

;------------------------------------------------------------------------------
; RUNADR reg, label: run-time address of label (r10 = image run address)
;------------------------------------------------------------------------------
        MACRO
        RUNADR  $reg, $label
        ldr     $reg, =$label
        sub     $reg, $reg, #S5L_IMAGE_VA_BASE
        add     $reg, $reg, r10
        MEND

;------------------------------------------------------------------------------
; PROBEMEM addr, string: print "string 0x<word at physical addr>"
; PROBEVAL string: print "string 0x<r0>"
;------------------------------------------------------------------------------
        MACRO
        PROBEMEM $addr, $str
        ldr     r0, =$addr
        ldr     r0, [r0]
        RUNADR  r1, $str
        bl      EarlyPutTag
        MEND

        MACRO
        PROBEVAL $str
        RUNADR  r1, $str
        bl      EarlyPutTag
        MEND

        STARTUPTEXT

;------------------------------------------------------------------------------
;
;   StartUp: main entry point
;
;------------------------------------------------------------------------------

        LEAF_ENTRY StartUp

        ;----------------------------------------------------------------------
        ; 1. Hand-over state for the probe (kept in r4-r9), then SVC mode
        ;    with IRQ and FIQ masked
        ;----------------------------------------------------------------------
        sub     r4, pc, #8                      ; run address of StartUp
        mrs     r6, cpsr
        mrc     p15, 0, r5, c1, c0, 0           ; SCTLR
        mrc     p15, 0, r7, c2, c0, 0           ; TTBR0

        mov     r0, #0xD3                       ; SVC mode (0x13) | I | F
        msr     cpsr_c, r0

        ;----------------------------------------------------------------------
        ; 2. Note and mask every VIC source, finish any in-service IRQ, stop
        ;    timers 0-7 and acknowledge their status. The 64-bit tick counter
        ;    (0x3C700080) keeps running: the OAL uses it.
        ;----------------------------------------------------------------------
        ldr     r1, =S5L_VIC0_PA
        ldr     r2, =S5L_VIC1_PA
        ldr     r8, [r1, #VIC_INTENABLE]
        ldr     r9, [r2, #VIC_INTENABLE]
        mvn     r0, #0
        str     r0, [r1, #VIC_INTENCLEAR]
        str     r0, [r2, #VIC_INTENCLEAR]
        mov     r0, #0
        str     r0, [r1, #VIC_ADDRESS]
        str     r0, [r2, #VIC_ADDRESS]

        ldr     r1, =S5L_TIMER_PA
        mov     r0, #0
        str     r0, [r1, #0x004]                ; timer 0 command: stop
        str     r0, [r1, #0x024]                ; timer 1
        str     r0, [r1, #0x044]                ; timer 2
        str     r0, [r1, #0x064]                ; timer 3
        str     r0, [r1, #0x0A4]                ; timer 4
        str     r0, [r1, #0x0C4]                ; timer 5
        str     r0, [r1, #0x0E4]                ; timer 6
        str     r0, [r1, #0x104]                ; timer 7
        ldr     r2, =S5L_TIMERIRQSTAT_PA
        ldr     r0, [r2]
        ldr     r3, [r1, #TIMER_IRQLATCH]       ; as openiBoot's timer IRQ handler
        str     r0, [r1, #TIMER_IRQLATCH]

        ;----------------------------------------------------------------------
        ; 3. Write back and invalidate the D-cache, then MMU, D-cache,
        ;    I-cache, branch prediction and high vectors off. Then invalidate
        ;    the I-cache (four times, ARM1136/ARM1176 erratum 411920 as Linux
        ;    does), the branch target cache and the TLBs.
        ;----------------------------------------------------------------------
        mov     r0, #0
        mcr     p15, 0, r0, c7, c14, 0          ; clean and invalidate D-cache
        mcr     p15, 0, r0, c7, c10, 4          ; data synchronization barrier

        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #CP15_CTRL_M
        bic     r0, r0, #CP15_CTRL_C
        bic     r0, r0, #CP15_CTRL_Z
        bic     r0, r0, #CP15_CTRL_I
        bic     r0, r0, #CP15_CTRL_V            ; low vectors; the kernel sets its own
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 4           ; prefetch flush

        mcr     p15, 0, r0, c7, c5, 0           ; invalidate I-cache
        mcr     p15, 0, r0, c7, c5, 0
        mcr     p15, 0, r0, c7, c5, 0
        mcr     p15, 0, r0, c7, c5, 0
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        mcr     p15, 0, r0, c7, c5, 6           ; flush branch target cache
        mcr     p15, 0, r0, c8, c7, 0           ; invalidate TLBs
        mcr     p15, 0, r0, c7, c10, 4          ; data synchronization barrier
        mcr     p15, 0, r0, c7, c5, 4           ; prefetch flush

        ; The I-cache works with the MMU off: turn it on to speed up the
        ; probe and the copy. It goes off again before KernelStart.
        mrc     p15, 0, r0, c1, c0, 0
        orr     r0, r0, #CP15_CTRL_I
        mcr     p15, 0, r0, c1, c0, 0

        ;----------------------------------------------------------------------
        ; 4. Early stack below the reserved top of DRAM, image run address,
        ;    screen blue, probe
        ;----------------------------------------------------------------------
        ldr     sp, =S5L_EARLY_STACK_TOP
        ldr     r0, =S5L_EARLY_FLAGS_PA
        mov     r1, #0
        str     r1, [r0]                        ; UART not known to be dead yet

        ldr     r0, =StartUp                    ; link VA of StartUp (romimage fixup)
        sub     r0, r0, #S5L_IMAGE_VA_BASE      ; offset of StartUp in the image
        sub     r10, r4, r0                     ; r10 = run address of the image
        ldr     r11, =S5L_DRAM_PA_BASE          ; r11 = link address of the image

        IF SCREEN_MARKS
        ldr     r0, =0x000000FF                 ; blue (RGB888)
        ldr     r1, =0x0000001F                 ; blue (RGB565)
        bl      ScreenFill
        ENDIF

        IF EARLY_UART
        bl      EarlyUartInit
        bl      ProbeDump
        ENDIF

        ;----------------------------------------------------------------------
        ; 5. Packed image (files\mkimg3.ps1 -Compress)? The pack header is in
        ;    the jump page: 'LZCE', packed length, head length, image length.
        ;    Then r4 = 1, r6 = packed length, r7 = head length, r8 = image
        ;    length. Otherwise r4 = 0 and r8 comes from the ROM header.
        ;----------------------------------------------------------------------
        mov     r4, #0
        ldr     r0, [r10, #PACK_HDR_MAGIC]
        ldr     r1, =PACK_MAGIC
        cmp     r0, r1
        bne     NotPacked
        ldr     r6, [r10, #PACK_HDR_PACKEDLEN]
        ldr     r7, [r10, #PACK_HDR_HEADLEN]
        ldr     r8, [r10, #PACK_HDR_IMAGELEN]
        ; 0 < head < image <= NK region, head word aligned, 0 < packed <= 16 MiB
        cmp     r7, #0
        beq     BadPack
        tst     r7, #3
        bne     BadPack
        cmp     r7, r8
        bhs     BadPack
        ldr     r0, =S5L_NK_MAXLEN
        cmp     r8, r0
        bhi     BadPack
        cmp     r6, #0
        beq     BadPack
        ldr     r0, =S5L_PACKED_MAXLEN
        cmp     r6, r0
        bhi     BadPack
        mov     r4, #1
        b       HaveLength

BadPack
        IF EARLY_UART
        RUNADR  r1, StrBadPack
        bl      EarlyPuts
        ENDIF
BadPackHang
        b       BadPackHang                     ; nothing sensible to start

        ;----------------------------------------------------------------------
        ;    Not packed: image length from the ROM header ('CECE' at offset
        ;    0x40, then the ROMHDR VA; ROMHDR.physlast = VA of the image end).
        ;    Fallback: the whole NK region.
        ;----------------------------------------------------------------------
NotPacked
        ldr     r8, =S5L_NK_MAXLEN
        ldr     r0, [r10, #ROM_SIGNATURE_OFFSET]
        ldr     r1, =ROM_SIGNATURE
        cmp     r0, r1
        bne     HaveLength
        ldr     r0, [r10, #(ROM_SIGNATURE_OFFSET + 4)]
        sub     r0, r0, #S5L_IMAGE_VA_BASE      ; ROMHDR offset in the image
        cmp     r0, r8
        bhs     HaveLength
        add     r0, r0, r10
        ldr     r0, [r0, #ROMHDR_PHYSLAST]
        sub     r0, r0, #S5L_IMAGE_VA_BASE      ; image length
        cmp     r0, #0
        beq     HaveLength
        cmp     r0, r8
        bhi     HaveLength
        add     r8, r0, #3
        bic     r8, r8, #3
HaveLength

        IF EARLY_UART
        mov     r0, r10
        PROBEVAL StrImageAt
        mov     r0, r8
        PROBEVAL StrImageLen
        mov     r0, r11
        PROBEVAL StrLinkAddr
        cmp     r4, #0
        beq     ProbeNotPacked
        mov     r0, r6
        PROBEVAL StrPackedLen
ProbeNotPacked
        ENDIF

        cmp     r4, #0
        bne     CopyPacked
        cmp     r10, r11
        beq     Relocated                       ; not packed and already at the link address
        b       PrepareStub

        ; Packed: move the packed data out of the way first. Unpacking
        ; writes up to 32 MB from PA 0x08000000, over the load area.
CopyPacked
        add     r0, r10, r7                     ; packed data follows the head
        ldr     r1, =S5L_PACKED_PA
        mov     r2, r6
CopyPackedLoop
        ldr     r3, [r0], #4
        str     r3, [r1], #4
        subs    r2, r2, #4
        bgt     CopyPackedLoop

        ; Copy the relocation stub out of the way (the image may overlap
        ; itself), then let it copy (and unpack) the image and jump to
        ; Relocated there.
PrepareStub
        RUNADR  r0, RelocStubStart
        RUNADR  r1, RelocStubEnd
        ldr     r2, =S5L_RELOC_STUB_PA
CopyStub
        ldr     r3, [r0], #4
        str     r3, [r2], #4
        cmp     r0, r1
        blo     CopyStub
        mov     r0, #0
        mcr     p15, 0, r0, c7, c10, 4          ; data synchronization barrier
        mcr     p15, 0, r0, c7, c5, 0           ; invalidate I-cache
        mcr     p15, 0, r0, c7, c5, 6           ; flush branch target cache
        mcr     p15, 0, r0, c7, c5, 4           ; prefetch flush

        mov     r0, r10                         ; source: where the image runs
        mov     r1, r11                         ; destination: its link address
        mov     r5, r6                          ; packed length (packed only)
        cmp     r4, #0
        moveq   r2, r8                          ; not packed: the whole image
        movne   r2, r7                          ; packed: the head
        ldrne   r4, =S5L_PACKED_PA              ; packed data (r4 = 0: none)
        ldr     r3, =Relocated
        sub     r3, r3, #S5L_IMAGE_VA_BASE
        add     r3, r3, r11                     ; Relocated at the link address
        ldr     r12, =S5L_RELOC_STUB_PA
        mov     pc, r12

        ;----------------------------------------------------------------------
        ; 6. At the link address: screen green, I-cache off, KernelStart
        ;----------------------------------------------------------------------
Relocated
        ldr     r10, =S5L_DRAM_PA_BASE          ; the image runs at its link address now

        IF SCREEN_MARKS
        ldr     r0, =0x0000FF00                 ; green (RGB888)
        ldr     r1, =0x000007E0                 ; green (RGB565)
        bl      ScreenFill
        ENDIF

        IF EARLY_UART
        RUNADR  r1, StrKernelStart
        bl      EarlyPuts
        bl      EarlyUartDrain
        ENDIF

        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #CP15_CTRL_I
        mcr     p15, 0, r0, c1, c0, 0
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 0           ; invalidate I-cache
        mcr     p15, 0, r0, c7, c5, 6           ; flush branch target cache
        mcr     p15, 0, r0, c7, c10, 4          ; data synchronization barrier
        mcr     p15, 0, r0, c7, c5, 4           ; prefetch flush

        ; physical address of OEMAddressTable (we run at our link PA)
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
;   RelocStub: copied to S5L_RELOC_STUB_PA and run from there.
;   r0 = source, r1 = destination, r2 = bytes to copy (multiple of 4, > 0),
;   r3 = address to jump to afterwards,
;   r4 = packed data (0 = not packed), r5 = packed length.
;   Copies r2 bytes (the whole image, or the head of a packed one), then
;   unpacks the LZ4 block behind the head and clears the pack header.
;   Uses r0-r12 except r3. Position independent: only relative branches.
;   Same steps as Img3.Lz4Decompress in files\mkimg3.ps1.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY RelocStub

RelocStubStart
        mov     r6, r1                          ; destination base
        mov     r7, r2                          ; bytes copied
        cmp     r1, r0
        beq     RelocCopied                     ; already in place
        bhi     RelocBackward                   ; destination above source: copy from the end
RelocForward
        ldr     r12, [r0], #4
        str     r12, [r1], #4
        subs    r2, r2, #4
        bgt     RelocForward
        b       RelocCopied
RelocBackward
        add     r0, r0, r2
        add     r1, r1, r2
RelocBackwardLoop
        ldr     r12, [r0, #-4]!
        str     r12, [r1, #-4]!
        subs    r2, r2, #4
        bgt     RelocBackwardLoop

RelocCopied
        cmp     r4, #0
        beq     RelocDone

        ; LZ4 block: r4 = input, r5 = input end, r1 = output
        add     r1, r6, r7                      ; output follows the head
        add     r5, r4, r5
LzSequence
        cmp     r4, r5
        bhs     LzEnd
        ldrb    r7, [r4], #1                    ; token
        mov     r8, r7, lsr #4                  ; literal length
        cmp     r8, #15
        bne     LzLiterals
LzLiteralMore
        ldrb    r9, [r4], #1
        add     r8, r8, r9
        cmp     r9, #255
        beq     LzLiteralMore
LzLiterals
        cmp     r8, #0
        beq     LzLiteralsDone
LzLiteralLoop
        ldrb    r9, [r4], #1
        strb    r9, [r1], #1
        subs    r8, r8, #1
        bne     LzLiteralLoop
LzLiteralsDone
        cmp     r4, r5
        bhs     LzEnd                           ; last sequence: literals only
        ldrb    r8, [r4], #1
        ldrb    r9, [r4], #1
        orr     r8, r8, r9, lsl #8              ; match offset
        and     r9, r7, #15                     ; match length - 4
        cmp     r9, #15
        bne     LzMatch
LzMatchMore
        ldrb    r10, [r4], #1
        add     r9, r9, r10
        cmp     r10, #255
        beq     LzMatchMore
LzMatch
        add     r9, r9, #4
        sub     r10, r1, r8                     ; match source (may overlap the output)
LzMatchLoop
        ldrb    r11, [r10], #1
        strb    r11, [r1], #1
        subs    r9, r9, #1
        bne     LzMatchLoop
        b       LzSequence
LzEnd
        ; the unpacked image has zeros where the pack header was
        mov     r12, #0
        str     r12, [r6, #PACK_HDR_MAGIC]
        str     r12, [r6, #PACK_HDR_PACKEDLEN]
        str     r12, [r6, #PACK_HDR_HEADLEN]
        str     r12, [r6, #PACK_HDR_IMAGELEN]

RelocDone
        mov     r12, #0
        mcr     p15, 0, r12, c7, c10, 4         ; data synchronization barrier
        mcr     p15, 0, r12, c7, c5, 0          ; invalidate I-cache
        mcr     p15, 0, r12, c7, c5, 6          ; flush branch target cache
        mcr     p15, 0, r12, c7, c5, 4          ; prefetch flush
        mov     pc, r3
RelocStubEnd

        ENTRY_END

;------------------------------------------------------------------------------
;
;   ScreenFill: fill display window 0 with one colour, MMU off.
;   r0 = colour for RGB888, r1 = colour for RGB565. Uses r0-r3, r12.
;
;   Window 0 registers as openiBoot sets them (clcd.c createWindow). Does
;   nothing unless the format is RGB888 or RGB565, the size is sane and the
;   framebuffer lies in the reserved top of DRAM (so it cannot hit the image
;   or CE's RAM).
;
;------------------------------------------------------------------------------

        LEAF_ENTRY ScreenFill

        ldr     r12, =S5L_CLCD_PA
        ldr     r2, [r12, #CLCD_W0_FORMAT]
        mov     r2, r2, lsr #8
        and     r2, r2, #0x0F
        cmp     r2, #7
        moveq   r2, #0                          ; RGB888: one pixel per word
        beq     ScreenFillSize
        cmp     r2, #4
        movne   pc, lr                          ; unknown format: leave the screen alone
        orr     r0, r1, r1, lsl #16             ; RGB565: two pixels per word
        mov     r2, #1
ScreenFillSize
        ldr     r3, [r12, #CLCD_W0_SIZE]
        mov     r3, r3, lsl #16
        mov     r3, r3, lsr #16                 ; height
        cmp     r3, #0
        moveq   pc, lr
        cmp     r3, #0x800
        movhi   pc, lr
        ldr     r1, [r12, #CLCD_W0_STRIDE]      ; pixels per line
        cmp     r1, #0
        moveq   pc, lr
        cmp     r1, #0x800
        movhi   pc, lr
        mul     r3, r1, r3                      ; pixels
        mov     r3, r3, lsr r2                  ; words
        ldr     r1, [r12, #CLCD_W0_ADDR]
        tst     r1, #3
        movne   pc, lr
        ldr     r12, =S5L_FB_RESERVED_PA
        cmp     r1, r12
        movlo   pc, lr
        add     r12, r1, r3, lsl #2             ; end of the framebuffer
        cmp     r12, #S5L_DRAM_PA_END
        movhi   pc, lr
ScreenFillLoop
        str     r0, [r1], #4
        subs    r3, r3, #1
        bne     ScreenFillLoop
        mov     pc, lr

        LTORG

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyUartInit: keep iBoot's UART0 set-up (clock, speed, pins); only make
;   sure Rx and Tx are in polling mode. Uses r0-r2.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyUartInit

        ldr     r1, =S5L_UART0_PA
        ldr     r0, [r1, #UART_UCON]
        and     r2, r0, #UCON_MODE_MASK
        cmp     r2, #UCON_MODE_POLL
        bicne   r0, r0, #UCON_MODE_MASK
        orrne   r0, r0, #UCON_MODE_POLL
        strne   r0, [r1, #UART_UCON]
        mov     pc, lr

        LTORG

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyPutc: write the byte in r0 to UART0 (polled, waits until the
;   transmitter is empty, so the time per byte is one character time).
;   If the UART does not drain, it is marked dead and later bytes are
;   dropped instead of hanging the boot. Uses r1-r3.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyPutc

        ldr     r1, =S5L_EARLY_FLAGS_PA
        ldr     r2, [r1]
        cmp     r2, #0
        movne   pc, lr                          ; UART dead: drop the byte
        ldr     r1, =S5L_UART0_PA
        ldr     r3, =EARLY_UART_TIMEOUT
EarlyPutcWait
        ldr     r2, [r1, #UART_UTRSTAT]
        tst     r2, #UTRSTAT_TX_EMPTY
        bne     EarlyPutcSend
        subs    r3, r3, #1
        bne     EarlyPutcWait
        ldr     r1, =S5L_EARLY_FLAGS_PA         ; timed out: give up on the UART
        mov     r2, #1
        str     r2, [r1]
        mov     pc, lr
EarlyPutcSend
        str     r0, [r1, #UART_UTXH]
        ; wait until the byte shows as pending: the status lags the write
        ; (see S5lUart0PutByte in s5l8720_uart.h)
        ldr     r3, =EARLY_UART_TIMEOUT
EarlyPutcPending
        ldr     r2, [r1, #UART_UTRSTAT]
        tst     r2, #UTRSTAT_TX_EMPTY
        moveq   pc, lr
        subs    r3, r3, #1
        bne     EarlyPutcPending
        mov     pc, lr

        LTORG

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyUartDrain: wait until the last byte has left (or the timeout).
;   Uses r1-r3.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyUartDrain

        ldr     r1, =S5L_EARLY_FLAGS_PA
        ldr     r2, [r1]
        cmp     r2, #0
        movne   pc, lr
        ldr     r1, =S5L_UART0_PA
        ldr     r3, =EARLY_UART_TIMEOUT
EarlyUartDrainWait
        ldr     r2, [r1, #UART_UTRSTAT]
        tst     r2, #UTRSTAT_TX_EMPTY
        movne   pc, lr
        subs    r3, r3, #1
        bne     EarlyUartDrainWait
        mov     pc, lr

        LTORG

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyPuts: write the NUL-terminated string at r1. Uses r0-r3.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyPuts

        stmfd   sp!, {r4, lr}
        mov     r4, r1
EarlyPutsLoop
        ldrb    r0, [r4], #1
        cmp     r0, #0
        beq     EarlyPutsDone
        bl      EarlyPutc
        b       EarlyPutsLoop
EarlyPutsDone
        ldmfd   sp!, {r4, pc}

        ENTRY_END

;------------------------------------------------------------------------------
;
;   EarlyPutTag: write the string at r1, then "0x", the value in r0 as
;   8 hex digits, and CR LF. Uses r0-r3.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY EarlyPutTag

        stmfd   sp!, {r4-r5, lr}
        mov     r4, r0
        bl      EarlyPuts
        mov     r0, #'0'
        bl      EarlyPutc
        mov     r0, #'x'
        bl      EarlyPutc
        mov     r5, #8
EarlyPutTagHex
        mov     r0, r4, lsr #28
        cmp     r0, #10
        addlo   r0, r0, #'0'
        addhs   r0, r0, #('a' - 10)
        bl      EarlyPutc
        mov     r4, r4, lsl #4
        subs    r5, r5, #1
        bne     EarlyPutTagHex
        mov     r0, #0x0D
        bl      EarlyPutc
        mov     r0, #0x0A
        bl      EarlyPutc
        ldmfd   sp!, {r4-r5, pc}

        ENTRY_END

;------------------------------------------------------------------------------
;
;   ProbeDump: print the hand-over state and the registers the BSP depends
;   on, MMU off. Inputs (kept): r4 = run address of StartUp, r5 = SCTLR,
;   r6 = CPSR, r7 = TTBR0, r8/r9 = VIC0/VIC1 INTENABLE at entry,
;   r10 = image run address.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY ProbeDump

        stmfd   sp!, {r4-r11, lr}

        RUNADR  r1, StrBanner
        bl      EarlyPuts

        ; state at entry
        mov     r0, r4
        PROBEVAL StrEntryPc
        mov     r0, r6
        PROBEVAL StrCpsr
        mov     r0, r5
        PROBEVAL StrSctlr
        mov     r0, r7
        PROBEVAL StrTtbr0
        mov     r0, r8
        PROBEVAL StrVic0En
        mov     r0, r9
        PROBEVAL StrVic1En

        ; CPU
        mrc     p15, 0, r0, c0, c0, 0           ; main ID
        PROBEVAL StrMidr
        mrc     p15, 0, r0, c0, c0, 1           ; cache type
        PROBEVAL StrCacheType
        mrc     p15, 0, r0, c1, c0, 2           ; coprocessor access
        PROBEVAL StrCpacr
        ; VFP present? cp10/cp11 access bits stick only if it exists
        mrc     p15, 0, r11, c1, c0, 2
        orr     r0, r11, #0x00F00000
        mcr     p15, 0, r0, c1, c0, 2
        mov     r0, #0
        mcr     p15, 0, r0, c7, c5, 4           ; prefetch flush
        mrc     p15, 0, r0, c1, c0, 2
        mcr     p15, 0, r11, c1, c0, 2          ; restore
        mov     r1, #0
        mcr     p15, 0, r1, c7, c5, 4
        PROBEVAL StrCpacrVfp
        mrc     p15, 0, r0, c15, c2, 4          ; peripheral port remap
        PROBEVAL StrPeriPort

        ; SoC
        PROBEMEM S5L_MIU_REMAP_PA, StrMiuRemap
        PROBEMEM S5L_CHIPINFO_PA, StrChipInfo
        PROBEMEM (S5L_CLK_PA + 0x00), StrClkCfg0
        PROBEMEM (S5L_CLK_PA + 0x04), StrClkCfg1
        PROBEMEM (S5L_CLK_PA + 0x08), StrClkCfg2
        PROBEMEM (S5L_CLK_PA + 0x20), StrPll0Con
        PROBEMEM (S5L_CLK_PA + 0x24), StrPll1Con
        PROBEMEM (S5L_CLK_PA + 0x28), StrPll2Con
        PROBEMEM (S5L_CLK_PA + 0x44), StrPllMode
        PROBEMEM (S5L_CLK_PA + 0x48), StrGates0
        PROBEMEM (S5L_CLK_PA + 0x4C), StrGates1
        PROBEMEM (S5L_CLK_PA + 0x58), StrGates2
        PROBEMEM (S5L_CLK_PA + 0x68), StrGates3
        PROBEMEM (S5L_CLK_PA + 0x6C), StrGates4

        PROBEMEM (S5L_UART0_PA + UART_ULCON), StrUlcon
        PROBEMEM (S5L_UART0_PA + UART_UCON), StrUcon
        PROBEMEM (S5L_UART0_PA + UART_UFCON), StrUfcon
        PROBEMEM (S5L_UART0_PA + UART_UMCON), StrUmcon
        PROBEMEM (S5L_UART0_PA + UART_UBAUD), StrUbaud

        PROBEMEM (S5L_CLCD_PA + 0x00), StrClcd000
        PROBEMEM (S5L_CLCD_PA + 0x04), StrClcd004
        PROBEMEM (S5L_CLCD_PA + 0x10), StrClcd010
        PROBEMEM (S5L_CLCD_PA + CLCD_W0_FORMAT), StrClcdFmt
        PROBEMEM (S5L_CLCD_PA + CLCD_W0_ADDR), StrClcdAddr
        PROBEMEM (S5L_CLCD_PA + CLCD_W0_STRIDE), StrClcdStride
        PROBEMEM (S5L_CLCD_PA + CLCD_W0_SIZE), StrClcdSize

        PROBEMEM S5L_TIMERIRQSTAT_PA, StrTimerStat
        PROBEMEM (S5L_TIMER_PA + 0x88), StrTimerCtrl
        PROBEMEM (S5L_TIMER_PA + TIMER_TICKSHIGH), StrTicksHigh
        PROBEMEM (S5L_TIMER_PA + TIMER_TICKSLOW), StrTicksLow

        ; Tick counter rate: ticks during 100 characters on the UART
        ; (each takes 10 bit times; 86.8 us at 115200 baud)
        mov     r0, #'.'
        bl      EarlyPutc
        bl      EarlyUartDrain
        ldr     r11, =S5L_TIMER_PA
        ldr     r5, [r11, #TIMER_TICKSLOW]
        mov     r6, #100
ProbeDots
        mov     r0, #'.'
        bl      EarlyPutc
        subs    r6, r6, #1
        bne     ProbeDots
        bl      EarlyUartDrain
        ldr     r7, [r11, #TIMER_TICKSLOW]
        mov     r0, #0x0D
        bl      EarlyPutc
        mov     r0, #0x0A
        bl      EarlyPutc
        sub     r0, r7, r5
        PROBEVAL StrTicks100

        ldmfd   sp!, {r4-r11, pc}

        LTORG

        ENTRY_END

;------------------------------------------------------------------------------
; Probe strings (each tag is padded to the same width)
;------------------------------------------------------------------------------

StrBanner       DCB     13, 10, "CE> iPod touch 2G hand-over probe (ipodtouch2g startup.s)", 13, 10, 0
StrKernelStart  DCB     "CE> KernelStart", 13, 10, 0
StrEntryPc      DCB     "entry pc    ", 0
StrCpsr         DCB     "cpsr        ", 0
StrSctlr        DCB     "sctlr       ", 0
StrTtbr0        DCB     "ttbr0       ", 0
StrVic0En       DCB     "vic0 enable ", 0
StrVic1En       DCB     "vic1 enable ", 0
StrMidr         DCB     "midr        ", 0
StrCacheType    DCB     "cache type  ", 0
StrCpacr        DCB     "cpacr       ", 0
StrCpacrVfp     DCB     "cpacr+vfp   ", 0
StrPeriPort     DCB     "periph port ", 0
StrMiuRemap     DCB     "miu remap   ", 0
StrChipInfo     DCB     "chipinfo    ", 0
StrClkCfg0      DCB     "clk config0 ", 0
StrClkCfg1      DCB     "clk config1 ", 0
StrClkCfg2      DCB     "clk config2 ", 0
StrPll0Con      DCB     "pll0con     ", 0
StrPll1Con      DCB     "pll1con     ", 0
StrPll2Con      DCB     "pll2con     ", 0
StrPllMode      DCB     "pllmode     ", 0
StrGates0       DCB     "clk gates0  ", 0
StrGates1       DCB     "clk gates1  ", 0
StrGates2       DCB     "clk gates2  ", 0
StrGates3       DCB     "clk gates3  ", 0
StrGates4       DCB     "clk gates4  ", 0
StrUlcon        DCB     "uart ulcon  ", 0
StrUcon         DCB     "uart ucon   ", 0
StrUfcon        DCB     "uart ufcon  ", 0
StrUmcon        DCB     "uart umcon  ", 0
StrUbaud        DCB     "uart ubaud  ", 0
StrClcd000      DCB     "clcd 000    ", 0
StrClcd004      DCB     "clcd 004    ", 0
StrClcd010      DCB     "clcd 010    ", 0
StrClcdFmt      DCB     "clcd w0 fmt ", 0
StrClcdAddr     DCB     "clcd w0 addr", 0
StrClcdStride   DCB     "clcd w0 line", 0
StrClcdSize     DCB     "clcd w0 size", 0
StrTimerStat    DCB     "timer stat  ", 0
StrTimerCtrl    DCB     "timer ctrl  ", 0
StrTicksHigh    DCB     "ticks high  ", 0
StrTicksLow     DCB     "ticks low   ", 0
StrTicks100     DCB     "ticks/100ch ", 0
StrImageAt      DCB     "image at    ", 0
StrImageLen     DCB     "image len   ", 0
StrLinkAddr     DCB     "link addr   ", 0
StrPackedLen    DCB     "packed len  ", 0
StrBadPack      DCB     "CE> bad pack header, stopped", 13, 10, 0
        ALIGN   4

;------------------------------------------------------------------------------
;
;   StartupVfpPresent: TRUE if the VFP coprocessor (cp10/cp11) exists, for
;   OEMInit. Callable from C with the MMU on; leaves CPACR as it was.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY StartupVfpPresent

        mrc     p15, 0, r1, c1, c0, 2
        orr     r0, r1, #0x00F00000
        mcr     p15, 0, r0, c1, c0, 2
        mov     r2, #0
        mcr     p15, 0, r2, c7, c5, 4           ; prefetch flush
        mrc     p15, 0, r0, c1, c0, 2
        mcr     p15, 0, r1, c1, c0, 2           ; restore
        mcr     p15, 0, r2, c7, c5, 4
        and     r0, r0, #0x00F00000
        cmp     r0, #0x00F00000
        moveq   r0, #1
        movne   r0, #0
        mov     pc, lr

        ENTRY_END

;------------------------------------------------------------------------------
;
;   StartupReadCpuId: main ID register, for OEMInit's log.
;
;------------------------------------------------------------------------------

        LEAF_ENTRY StartupReadCpuId

        mrc     p15, 0, r0, c0, c0, 0
        mov     pc, lr

        ENTRY_END

        END
