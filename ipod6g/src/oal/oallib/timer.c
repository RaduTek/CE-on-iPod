//
// timer.c - system tick and performance counter for the ipod6g BSP
//
// Timer B (16-bit): fixed 1 ms tick. ECLK 12 MHz / 16 / (TBPRE+1 = 75)
//   = 10 kHz, TBDATA0 = 10 -> 1 ms. Same setup as Rockbox tick_start()
//   (firmware/target/arm/s5l8702/kernel-s5l8702.c).
// Timer E (32-bit): free-running 1 MHz up-counter (Rockbox usec timer,
//   clocking-s5l8702.c), extended to 64 bits for QueryPerformanceCounter.
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>
#include <oal.h>

#include <bsp.h>

#define TB_PRESCALE         (S5L_OSC0_HZ / 16 / 10000 - 1)      // 74 -> 10 kHz
#define TB_COUNTS_PER_MS    10
#define TE_HZ               1000000

// 64-bit extension of timer E, maintained by the tick ISR
static volatile UINT32 g_perfHigh = 0;
static volatile UINT32 g_perfLastLow = 0;

// ---------------------------------------------------------------------------
// OALTimerInit: start timer E (perf counter) and timer B (1 ms tick)
//
BOOL OALTimerInit(UINT32 msecPerSysTick, UINT32 countsPerMSec, UINT32 countsMargin)
{
    UNREFERENCED_PARAMETER(countsMargin);

    // timers are clock gate APB bit 5; Rockbox leaves it ungated, make sure
    S5L_REG(S5L_PWRCON1_PA) &= ~S5L_PWRCON1_TIMER;

    // Timer E: ECLK / 1 / 12 = 1 MHz, interval mode up to 0xFFFFFFFF
    S5L_REG(S5L_TECON_PA)   = TCON_CS(4) | TCON_ECLK | TCON_MODE_INTERVAL;
    S5L_REG(S5L_TEPRE_PA)   = (S5L_OSC0_HZ / TE_HZ) - 1;
    S5L_REG(S5L_TEDATA0_PA) = 0xFFFFFFFF;
    S5L_REG(S5L_TECMD_PA)   = TCMD_CLR | TCMD_EN;

    g_perfHigh = 0;
    g_perfLastLow = 0;

    // Timer B: 10 kHz, interrupt 0 at TBDATA0 counts
    S5L_REG(S5L_TBCMD_PA)   = TCMD_CLR;
    S5L_REG(S5L_TBPRE_PA)   = TB_PRESCALE;
    S5L_REG(S5L_TBCON_PA)   = TCON_INT0_EN | TCON_CS(2) | TCON_ECLK | TCON_MODE_INTERVAL;
    S5L_REG(S5L_TBDATA0_PA) = TB_COUNTS_PER_MS * msecPerSysTick;
    S5L_REG(S5L_TBCMD_PA)   = TCMD_EN;

    // Idle time accounting (perf counter counts per ms)
    idleconv = countsPerMSec;
    curridlehigh = 0;
    curridlelow = 0;

    OEMInterruptEnable(SYSINTR_RESCHED, NULL, 0);

    OALMSG(1, (L"OALTimerInit: tick %d ms (timer B), perf counter 1 MHz (timer E)\r\n",
               msecPerSysTick));
    return TRUE;
}

// ---------------------------------------------------------------------------
// OALTimerTickIsr: called from OEMInterruptHandler on IRQ 8
//
void OALTimerTickIsr(void)
{
    UINT32 low;

    // acknowledge timer B (write back clears the pending status bits)
    S5L_REG(S5L_TBCON_PA) = S5L_REG(S5L_TBCON_PA);

    CurMSec++;

    // extend timer E to 64 bits
    low = S5L_REG(S5L_TECNT_PA);
    if (low < g_perfLastLow)
        g_perfHigh++;
    g_perfLastLow = low;
}

// ---------------------------------------------------------------------------
// OEMQueryPerformanceCounter / Frequency
//
BOOL OEMQueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount)
{
    UINT32 high, last, low;

    if (lpPerformanceCount == NULL)
        return FALSE;

    // consistent snapshot: retry if the tick ISR bumped the high word meanwhile
    do
    {
        high = g_perfHigh;
        last = g_perfLastLow;
        low = S5L_REG(S5L_TECNT_PA);
    } while (high != g_perfHigh);

    // timer E wrapped since the last tick, the ISR has not counted it yet
    if (low < last)
        high++;

    lpPerformanceCount->HighPart = (LONG)high;
    lpPerformanceCount->LowPart = low;
    return TRUE;
}

BOOL OEMQueryPerformanceFrequency(LARGE_INTEGER *lpFrequency)
{
    if (lpFrequency == NULL)
        return FALSE;

    lpFrequency->HighPart = 0;
    lpFrequency->LowPart = TE_HZ;
    return TRUE;
}

// ---------------------------------------------------------------------------
// OEMGetTickCount: REQUIRED
//
DWORD OEMGetTickCount(void)
{
    return CurMSec;
}

// ---------------------------------------------------------------------------
// Fixed tick: nothing to reprogram
//
VOID OEMUpdateReschedTime(DWORD dwTick)
{
    UNREFERENCED_PARAMETER(dwTick);
    return;
}

void OEMRefreshWatchDog(void)
{
    return;
}

void OEMProfilerTimerEnable(DWORD dwUSec)
{
    UNREFERENCED_PARAMETER(dwUSec);
    return;
}

void OEMProfilerTimerDisable(void)
{
    return;
}
