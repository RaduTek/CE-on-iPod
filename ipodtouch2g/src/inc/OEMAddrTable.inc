;------------------------------------------------------------------------------
;
;  File:  OEMAddrTable.inc
;
;  Defines g_oalAddressTable, passed to KernelStart to establish the
;  physical to virtual mapping. Each entry is mapped cached at its VA and
;  uncached at VA + 0x20000000.
;
;  iPod touch 2G (S5L8720):
;     VA 0x80000000 -> PA 0x08000000, 128 MB DRAM
;     VA 0x88000000 -> PA 0x38000000, 128 MB I/O (use 0xA8000000 uncached)
;
;  The CEonPi notes warn against mapping the BCM2835 registers here (the
;  kernel clears the shared bit on one core, which upset the Pi's GPU). The
;  Microsoft SMDK6410 BSP, also an ARM1176, maps its I/O in this table, so
;  this BSP does the same until the hardware says otherwise.
;
;------------------------------------------------------------------------------

        EXPORT  g_oalAddressTable[DATA]

;------------------------------------------------------------------------------
; TABLE FORMAT
;       cached address, physical address, size (MB)
;------------------------------------------------------------------------------

g_oalAddressTable

        DCD     0x80000000, 0x08000000, 128     ; DRAM
        DCD     0x88000000, 0x38000000, 128     ; S5L8720 I/O 0x38000000-0x3FFFFFFF
        DCD     0x00000000, 0x00000000,   0     ; end of table

;------------------------------------------------------------------------------

        END
