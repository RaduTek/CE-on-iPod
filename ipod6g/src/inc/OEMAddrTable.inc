;------------------------------------------------------------------------------
;
;  File:  OEMAddrTable.inc
;
;  Defines g_oalAddressTable, passed to KernelStart to establish the
;  physical to virtual mapping. Each entry is mapped cached at its VA and
;  uncached at VA + 0x20000000.
;
;  iPod classic (S5L8702):
;     VA 0x80000000 -> PA 0x08000000,  64 MB SDRAM
;     VA 0x84000000 -> PA 0x38000000, 128 MB I/O (use 0xA4000000 uncached)
;
;  Unlike the ARMv6 Pi port, the I/O block can be mapped here: the ARMv5
;  page tables have no shared bit to worry about.
;
;------------------------------------------------------------------------------

        EXPORT  g_oalAddressTable[DATA]

;------------------------------------------------------------------------------
; TABLE FORMAT
;       cached address, physical address, size (MB)
;------------------------------------------------------------------------------

g_oalAddressTable

        DCD     0x80000000, 0x08000000,  64     ; SDRAM
        DCD     0x84000000, 0x38000000, 128     ; S5L8702 I/O 0x38000000-0x3FFFFFFF
        DCD     0x00000000, 0x00000000,   0     ; end of table

;------------------------------------------------------------------------------

        END
