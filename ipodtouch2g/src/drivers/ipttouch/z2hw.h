//
// z2hw.h - hardware layer of the touch driver: SPI4, the touch pins, delays
//
// As verified by the OAL probe (src\oal\oallib\touchprobe.c, step 3,
// 2026-11-28): SPI4 in mode 3 with the fixed 24 MHz clock, polled FIFOs,
// chip select, reset and data ready are GPIOs (see s5l8720.h).
//
#ifndef __Z2HW_H
#define __Z2HW_H

#ifdef __cplusplus
extern "C" {
#endif

// Map the registers; FALSE if a mapping failed or SPI4's clock is off
BOOL  Z2Hw_Init(void);

// Busy wait (the 12 MHz performance counter)
void  Z2Hw_DelayUs(DWORD us);

// Reset: TRUE = held in reset (the pin is active-high)
void  Z2Hw_Reset(BOOL fInReset);

// Data ready pin level: 0 = the controller has something (active-low)
DWORD Z2Hw_DataReady(void);

// One transfer with chip select low around it, then delayAfterUs.
// pRx may be NULL. FALSE on an SPI timeout.
BOOL  Z2Hw_Xfer(const BYTE *pTx, BYTE *pRx, DWORD len, DWORD hz, DWORD delayAfterUs);

// Diagnostics of the last transfer
DWORD Z2Hw_LastStatus(void);

// Receive FIFO bytes thrown away since the start: left over before a
// transfer, and beyond the expected length after one
typedef struct
{
    DWORD   staleBefore;
    DWORD   extraAfter;
    BYTE    firstStale;
    BYTE    firstExtra;
} Z2HW_STATS;

void  Z2Hw_GetStats(Z2HW_STATS *pStats);

#ifdef __cplusplus
}
#endif

#endif // __Z2HW_H
