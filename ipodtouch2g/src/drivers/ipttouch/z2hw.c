//
// z2hw.c - hardware layer of the touch driver: SPI4, the touch pins, delays
//
// SPI4 programming follows n72-linux's spi-s5l8720.c (polled segment):
// clear status, reset both FIFOs, set the RX count and TX length, preload
// up to 16 words, enable RX/TX, then move words while keeping no more than
// 16 in flight. Verified on the device by the OAL probe (step 3). Pins are
// driven through the GPIO function-select register as openiBoot does.
//
#include <windows.h>
#include <ceddk.h>

#include <s5l8720.h>
#include "z2hw.h"

// SPI registers and bits (see also s5l8720.h)
#define SPICTL_ENABLE           (1 << 0)
#define SPICTL_RESET_TX         (1 << 2)
#define SPICTL_RESET_RX         (1 << 3)
#define SPISETUP_MODE3_8BIT     ((1 << 14) | (3 << 3) | (1 << 2) | (1 << 1))    // fixed clock, master, CPOL, CPHA
#define SPISETUP_OPTION5        (1 << 5)
#define SPISETUP_RX_ENABLE      (1 << 7)
#define SPISETUP_ENABLE2        (1 << 8)
#define SPISETUP_TX_ENABLE      (1 << 21)
#define SPISTATUS_CLEAR         0x0040000F
#define SPISTATUS_TXLVL(s)      (((s) >> 6) & 0x1F)
#define SPISTATUS_RXLVL(s)      (((s) >> 11) & 0x1F)
#define SPI_FIFO_DEPTH          16
#define SPI_MAX_WORDS           0xFFFF
#define SPI_CLOCK_HZ            24000000
#define SPI_MAX_DIV             0x3FF

#define CS_DELAY_US             10      // chip select setup and hold
#define PROGRESS_US             20000   // no FIFO progress for this long = timeout
#define EXTRA_WAIT_US           200     // after a transfer: room for one more byte at 83 kHz

static volatile UINT32 *g_pGpio = NULL;
static volatile UINT32 *g_pSpi = NULL;
static volatile UINT32 *g_pClk = NULL;
static LARGE_INTEGER g_qpcFreq;
static DWORD g_lastStatus = 0;
static Z2HW_STATS g_stats;

#define GPIO_REG(off)   (g_pGpio[(off) >> 2])
#define SPI_REG(off)    (g_pSpi[(off) >> 2])
#define CLK_REG(off)    (g_pClk[(off) >> 2])

static volatile UINT32 *Map(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return (volatile UINT32 *)MmMapIoSpace(a, 0x1000, FALSE);
}

static void PinOut(UINT32 pin, BOOL high)
{
    GPIO_REG(S5L_GPIO_FSEL_PA - S5L_GPIO_PA) =
        GPIO_FSEL_VALUE(pin, high ? GPIO_FSEL_OUT_HIGH : GPIO_FSEL_OUT_LOW);
}

static DWORD PinLevel(UINT32 pin)
{
    return (GPIO_REG(GPIO_PIN_PORT(pin) * 0x20 + GPIO_DAT) >> GPIO_PIN_BIT(pin)) & 1;
}

static UINT64 Now(void)
{
    LARGE_INTEGER t;

    QueryPerformanceCounter(&t);
    return (UINT64)t.QuadPart;
}

static UINT64 UsToCounts(DWORD us)
{
    return ((UINT64)us * (UINT64)g_qpcFreq.QuadPart) / 1000000;
}

BOOL Z2Hw_Init(void)
{
    g_pGpio = Map(S5L_GPIO_PA);
    g_pSpi = Map(S5L_SPI4_PA);
    g_pClk = Map(S5L_CLK_PA);
    if (g_pGpio == NULL || g_pSpi == NULL || g_pClk == NULL)
    {
        RETAILMSG(1, (L"ipttouch: MmMapIoSpace failed\r\n"));
        return FALSE;
    }
    if (!QueryPerformanceFrequency(&g_qpcFreq) || g_qpcFreq.QuadPart == 0)
    {
        RETAILMSG(1, (L"ipttouch: no performance counter\r\n"));
        return FALSE;
    }

    // SPI4 clock gates (gates2 bit 4, gates4 bit 20; 1 = off): iBoot leaves
    // them on. Switch them on anyway (clearing a gate bit is harmless).
    CLK_REG(S5L_CLKGATES2_PA - S5L_CLK_PA) &= ~SPI4_GATES2_BIT;
    CLK_REG(S5L_CLKGATES4_PA - S5L_CLK_PA) &= ~SPI4_GATES4_BIT;

    // as Linux's prepare_message: mode 3 idle clock before any chip select
    SPI_REG(SPI_SETUP) = SPISETUP_MODE3_8BIT;
    SPI_REG(SPI_PIN) = 2;
    SPI_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    SPI_REG(SPI_CONTROL) = SPICTL_ENABLE;
    PinOut(TOUCH_GPIO_CS, TRUE);
    Z2Hw_DelayUs(100);
    return TRUE;
}

void Z2Hw_DelayUs(DWORD us)
{
    UINT64 end = Now() + UsToCounts(us);

    while (Now() < end)
        ;
}

void Z2Hw_Reset(BOOL fInReset)
{
    PinOut(TOUCH_GPIO_RESET, fInReset);
}

DWORD Z2Hw_DataReady(void)
{
    return PinLevel(TOUCH_GPIO_ATN);
}

DWORD Z2Hw_LastStatus(void)
{
    return g_lastStatus;
}

// One segment of at most SPI_MAX_WORDS bytes (8-bit words)
static BOOL Segment(const BYTE *pTx, BYTE *pRx, DWORD len)
{
    UINT32 setup = SPISETUP_MODE3_8BIT;
    DWORD txSent, rxDone = 0, st = 0, avail, inFlight, space, push, i, b;
    UINT64 end;

    SPI_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    SPI_REG(SPI_SETUP) = setup;
    SPI_REG(SPI_CONTROL) = SPICTL_ENABLE | SPICTL_RESET_TX | SPICTL_RESET_RX;

    // Anything still in the receive FIFO would shift this transfer's bytes
    // (seen 2026-11-28: frames one byte late after the first 2 MHz reads)
    for (i = 0; i < 2 * SPI_FIFO_DEPTH && SPISTATUS_RXLVL(SPI_REG(SPI_STATUS)) != 0; i++)
    {
        b = SPI_REG(SPI_RXDATA);
        if (g_stats.staleBefore == 0)
            g_stats.firstStale = (BYTE)b;
        g_stats.staleBefore++;
    }

    SPI_REG(SPI_CNT) = len;
    SPI_REG(SPI_TXBUFFERLEN) = len;
    SPI_REG(SPI_SETUP) = setup | SPISETUP_OPTION5;

    txSent = (len < SPI_FIFO_DEPTH) ? len : SPI_FIFO_DEPTH;
    for (i = 0; i < txSent; i++)
        SPI_REG(SPI_TXDATA) = pTx[i];
    SPI_REG(SPI_SETUP) = setup | SPISETUP_OPTION5 | SPISETUP_RX_ENABLE |
                         SPISETUP_ENABLE2 | SPISETUP_TX_ENABLE;

    end = Now() + UsToCounts(PROGRESS_US);
    while (rxDone < len)
    {
        st = SPI_REG(SPI_STATUS);

        avail = SPISTATUS_RXLVL(st);
        if (avail > len - rxDone)
            avail = len - rxDone;
        for (i = 0; i < avail; i++)
        {
            b = SPI_REG(SPI_RXDATA);
            if (pRx != NULL)
                pRx[rxDone] = (BYTE)b;
            rxDone++;
        }

        inFlight = txSent - rxDone;
        space = SPI_FIFO_DEPTH - SPISTATUS_TXLVL(st);
        if (space > SPI_FIFO_DEPTH - inFlight)
            space = SPI_FIFO_DEPTH - inFlight;
        push = len - txSent;
        if (push > space)
            push = space;
        for (i = 0; i < push; i++)
            SPI_REG(SPI_TXDATA) = pTx[txSent + i];
        txSent += push;

        if (avail != 0 || push != 0)
        {
            end = Now() + UsToCounts(PROGRESS_US);
            continue;
        }
        if (Now() > end)
        {
            g_lastStatus = st;
            SPI_REG(SPI_SETUP) = setup;
            SPI_REG(SPI_STATUS) = SPISTATUS_CLEAR;
            return FALSE;
        }
    }

    // bytes beyond the expected length (the controller clocking one more?)
    Z2Hw_DelayUs(EXTRA_WAIT_US);
    for (i = 0; i < 2 * SPI_FIFO_DEPTH && SPISTATUS_RXLVL(SPI_REG(SPI_STATUS)) != 0; i++)
    {
        b = SPI_REG(SPI_RXDATA);
        if (g_stats.extraAfter == 0)
            g_stats.firstExtra = (BYTE)b;
        g_stats.extraAfter++;
    }

    g_lastStatus = st;
    SPI_REG(SPI_SETUP) = setup;
    SPI_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    return TRUE;
}

void Z2Hw_GetStats(Z2HW_STATS *pStats)
{
    *pStats = g_stats;
}

BOOL Z2Hw_Xfer(const BYTE *pTx, BYTE *pRx, DWORD len, DWORD hz, DWORD delayAfterUs)
{
    DWORD div, seg;
    BOOL ok = TRUE;

    if (len == 0)
        return TRUE;

    div = (SPI_CLOCK_HZ + hz - 1) / hz;
    if (div < 1) div = 1;
    if (div > SPI_MAX_DIV) div = SPI_MAX_DIV;
    SPI_REG(SPI_CLKDIVIDER) = div;
    SPI_REG(SPI_IDD) = 0;

    PinOut(TOUCH_GPIO_CS, FALSE);
    Z2Hw_DelayUs(CS_DELAY_US);

    while (len > 0 && ok)
    {
        seg = (len > SPI_MAX_WORDS) ? SPI_MAX_WORDS : len;
        ok = Segment(pTx, pRx, seg);
        pTx += seg;
        if (pRx != NULL)
            pRx += seg;
        len -= seg;
    }

    Z2Hw_DelayUs(CS_DELAY_US);
    PinOut(TOUCH_GPIO_CS, TRUE);
    if (delayAfterUs >= 2000)
        Sleep((delayAfterUs + 999) / 1000);
    else if (delayAfterUs > 0)
        Z2Hw_DelayUs(delayAfterUs);
    return ok;
}
