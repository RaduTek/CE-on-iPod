//
// timer.c - system tick and performance counter for the ipodtouch2g BSP
//
// Following openiBoot (plat-s5l8720/timer.c, event.c, clock.c):
//   - the 64-bit free-running tick counter (0x3C700080/84) runs at
//     TicksPerSec = base clock / 2 (24 MHz / 2 if the chip's security epoch
//     is 1, else 12 MHz / 2). It is the performance counter.
//   - timer 4 is openiBoot's event timer: fixed clock, divider /2 (the same
//     rate as the tick counter), interval mode, interrupts on IRQ 7. Here
//     it gives the 1 ms system tick.
// None of this is verified on the device yet. The hand-over probe prints
// the tick counter rate ("ticks/100ch"), and the first tick interrupts log
// the timer status they see.
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>
#include <oal.h>

#include <bsp.h>

#define TICK_TIMER          4

// openiBoot's clock gate 0x13 (TIMER_CLOCKGATE): bits in gate registers 1, 2, 4
#define TIMER_GATES1_BITS   0x1F800020
#define TIMER_GATES2_BITS   0x00000060
#define TIMER_GATES4_BITS   0x00C0007F

static UINT32 g_ticksPerSec = 0;
static UINT32 g_tickIrqsLogged = 0;

// ---------------------------------------------------------------------------
// OALTimerTicksPerMs: tick counter counts per millisecond
//
UINT32 OALTimerTicksPerMs(void)
{
    if (g_ticksPerSec == 0)
        g_ticksPerSec = S5lTicksPerSec();
    return g_ticksPerSec / 1000;
}

// ---------------------------------------------------------------------------
// OALTimerInit: make sure the tick counter runs, start timer 4 (1 ms tick)
//
BOOL OALTimerInit(UINT32 msecPerSysTick, UINT32 countsPerMSec, UINT32 countsMargin)
{
    UINT32 base = S5L_TIMERn_PA(TICK_TIMER);
    UINT32 t0, t1 = 0, i;

    UNREFERENCED_PARAMETER(countsMargin);

    // timer clocks on (iBoot uses the timers, so they should be on already)
    S5L_REG(S5L_CLKGATES1_PA) &= ~TIMER_GATES1_BITS;
    S5L_REG(S5L_CLKGATES2_PA) &= ~TIMER_GATES2_BITS;
    S5L_REG(S5L_CLKGATES4_PA) &= ~TIMER_GATES4_BITS;

    if (g_ticksPerSec == 0)
        g_ticksPerSec = S5lTicksPerSec();

    // The tick counter should still run from iBoot. If it does not, start
    // it the way openiBoot's timer_init_rtc does.
    t0 = S5L_REG(S5L_TICKSLOW_PA);
    for (i = 0; i < 100000; i++)
    {
        t1 = S5L_REG(S5L_TICKSLOW_PA);
        if (t1 != t0)
            break;
    }
    if (t1 == t0)
    {
        OALMSG(1, (L"OALTimerInit: tick counter stopped, starting it\r\n"));
        S5L_REG(S5L_TIMERCTRL_PA) = 0x0000000A;
        S5L_REG(S5L_TIMERUNK2_PA) = 0xFFFFFFFF;
        S5L_REG(S5L_TIMERUNK1_PA) = 0xFFFFFFFF;
        S5L_REG(S5L_TIMERUNK4_PA) = 0xFFFFFFFF;
        S5L_REG(S5L_TIMERUNK3_PA) = 0xFFFFFFFF;
        S5L_REG(S5L_TIMERCTRL_PA) = 0x00018010;
    }

    // Timer 4: fixed clock / 2, interval mode, interrupts on (openiBoot
    // timer_init + timer_on_off)
    S5L_REG(base + TIMER_CMD)   = TCMD_STOP;
    S5L_REG(base + TIMER_CON)   = TCON_INT_EN | TCON_DIV(0) | TCON_FIXEDCLK;
    S5L_REG(base + TIMER_DATA0) = countsPerMSec * msecPerSysTick;
    S5L_REG(base + TIMER_DATA1) = 0;
    S5L_REG(base + TIMER_PRE)   = 0;
    S5L_REG(base + TIMER_CMD)   = TCMD_UPDATE;
    S5L_REG(base + TIMER_CMD)   = TCMD_START;

    // Idle time accounting (perf counter counts per ms)
    idleconv = countsPerMSec;
    curridlehigh = 0;
    curridlelow = 0;

    OEMInterruptEnable(SYSINTR_RESCHED, NULL, 0);

    OALMSG(1, (L"OALTimerInit: tick %d ms (timer 4, %d counts), counter %d Hz (chipinfo 0x%08x)\r\n",
               msecPerSysTick, countsPerMSec * msecPerSysTick, g_ticksPerSec,
               S5L_REG(S5L_CHIPINFO_PA)));
    return TRUE;
}

// ---------------------------------------------------------------------------
// OALTimerTickIsr: called from OEMInterruptHandler on IRQ 7
//
// Acknowledges the timers as openiBoot's timerIRQHandler does. Only timer 4
// runs (startup.s stops the others), so every IRQ 7 counts as a tick; the
// first few log the status bits, to check which bits timer 4 sets.
//
BOOL OALTimerTickIsr(void)
{
    UINT32 stat = S5L_REG(S5L_TIMERIRQSTAT_PA);

    (void)S5L_REG(S5L_TIMERIRQLATCH_PA);        // "being handled"
    if (stat & TIMERIRQSTAT_SPECIAL0)
        S5L_REG(S5L_TIMERCTRL_PA) |= TIMERIRQSTAT_SPECIAL0;
    if (stat & TIMERIRQSTAT_SPECIAL1)
        S5L_REG(S5L_TIMERCTRL_PA) |= TIMERIRQSTAT_SPECIAL1;
    S5L_REG(S5L_TIMERIRQLATCH_PA) = stat;       // "handled"

#ifndef SERIAL_KITL
    // raw UART builds only: in KITL builds the message would go through
    // KITL from interrupt context
    if (g_tickIrqsLogged < 3)
    {
        g_tickIrqsLogged++;
        OALMSG(1, (L"OALTimerTickIsr: timer status 0x%08x (timer 4 bits 0x%02x)\r\n",
                   stat, (stat >> TIMERIRQSTAT_SHIFT(TICK_TIMER)) & 0xFF));
    }
#endif

    CurMSec += RESCHED_PERIOD;
    return TRUE;
}

// ---------------------------------------------------------------------------
// OEMQueryPerformanceCounter / Frequency: the 64-bit tick counter
//
BOOL OEMQueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount)
{
    if (lpPerformanceCount == NULL)
        return FALSE;

    lpPerformanceCount->QuadPart = (LONGLONG)S5lReadTicks();
    return TRUE;
}

BOOL OEMQueryPerformanceFrequency(LARGE_INTEGER *lpFrequency)
{
    if (lpFrequency == NULL)
        return FALSE;

    lpFrequency->HighPart = 0;
    lpFrequency->LowPart = g_ticksPerSec;
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
