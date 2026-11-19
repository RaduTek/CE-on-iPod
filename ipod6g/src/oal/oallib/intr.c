//
// intr.c - interrupt handling for the ipod6g BSP (S5L8702, two PL192 VICs)
//
// Static SYSINTR <-> IRQ mapping (as in the CEonPi BSP):
//   SYSINTR_RESCHED  <-> S5L_IRQ_TIMER (8), timer B 1 ms tick
//   SYSINTR_UART0    <-> S5L_IRQ_UART0 (24)
//
// VIC usage follows Rockbox (firmware/target/arm/s5l8702/system-s5l8702.c):
// read both VICxADDRESS at entry, dispatch from VICxIRQSTATUS, then write 0
// to both VICxADDRESS.
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>

#include <bsp.h>

void OALTimerTickIsr(void);     // timer.c: acknowledges timer B, advances CurMSec

//------------------------------------------------------------------------------
// Local helpers
//
#define VIC0_REG(off)   S5L_REG(S5L_VIC0_PA + (off))
#define VIC1_REG(off)   S5L_REG(S5L_VIC1_PA + (off))

static void IrqEnable(UINT32 irq)
{
    if (irq < 32)
        VIC0_REG(VIC_INTENABLE) = 1u << irq;
    else if (irq < S5L_IRQ_MAX)
        VIC1_REG(VIC_INTENABLE) = 1u << (irq - 32);
}

static void IrqDisable(UINT32 irq)
{
    if (irq < 32)
        VIC0_REG(VIC_INTENCLEAR) = 1u << irq;
    else if (irq < S5L_IRQ_MAX)
        VIC1_REG(VIC_INTENCLEAR) = 1u << (irq - 32);
}

// Static SYSINTR -> IRQ table. Returns S5L_IRQ_MAX for an unknown SYSINTR.
static UINT32 SysIntrToIrq(DWORD dwSysIntr)
{
    switch (dwSysIntr)
    {
    case SYSINTR_RESCHED:   return S5L_IRQ_TIMER;
    case SYSINTR_UART0:     return S5L_IRQ_UART0;
    default:                return S5L_IRQ_MAX;
    }
}

// Kernel-defined SYSINTRs below SYSINTR_FIRMWARE that have no hardware IRQ
// on this board (SYSINTR_IST_BGT, SYSINTR_PROFILE, SYSINTR_TIMING,
// SYSINTR_RTC_ALARM, ...). The kernel and device manager call
// InterruptInitialize on some of them (SYSINTR_IST_BGT = 11 at boot); accept
// them as no-ops instead of failing.
static BOOL IsSoftwareSysIntr(DWORD dwSysIntr)
{
    return (dwSysIntr != SYSINTR_RESCHED) && (dwSysIntr < SYSINTR_FIRMWARE);
}

// ---------------------------------------------------------------------------
// OALIntrInit: mask and reset both VICs
//
BOOL OALIntrInit(void)
{
    VIC0_REG(VIC_INTENCLEAR) = 0xFFFFFFFF;
    VIC1_REG(VIC_INTENCLEAR) = 0xFFFFFFFF;

    // all sources are IRQ, none FIQ
    VIC0_REG(VIC_INTSELECT) = 0;
    VIC1_REG(VIC_INTSELECT) = 0;

    // no software interrupts pending
    VIC0_REG(VIC_SOFTINTCLEAR) = 0xFFFFFFFF;
    VIC1_REG(VIC_SOFTINTCLEAR) = 0xFFFFFFFF;

    // finish any interrupt left in service by the bootloader
    VIC0_REG(VIC_ADDRESS) = 0;
    VIC1_REG(VIC_ADDRESS) = 0;

    // edge config as Rockbox vic_init() leaves it
    S5L_REG(S5L_VIC0EDGE1_PA) = 0xFFFFFFFF;
    S5L_REG(S5L_VIC1EDGE1_PA) = 0xFFFFFFFF;

    OALMSG(OAL_INTR, (L"OALIntrInit: VICs masked\r\n"));
    return TRUE;
}

// ---------------------------------------------------------------------------
// OEMInterruptEnable: REQUIRED
//
BOOL OEMInterruptEnable(DWORD dwSysIntr, LPVOID pvData, DWORD cbData)
{
    UINT32 irq = SysIntrToIrq(dwSysIntr);

    UNREFERENCED_PARAMETER(pvData);
    UNREFERENCED_PARAMETER(cbData);

    if (IsSoftwareSysIntr(dwSysIntr))
        return TRUE;

    if (irq >= S5L_IRQ_MAX)
    {
        OALMSG(1, (L"OEMInterruptEnable: unknown SysIntr %d\r\n", dwSysIntr));
        return FALSE;
    }
    IrqEnable(irq);
    return TRUE;
}

// ---------------------------------------------------------------------------
// OEMInterruptDisable: REQUIRED
//
void OEMInterruptDisable(DWORD dwSysIntr)
{
    UINT32 irq = SysIntrToIrq(dwSysIntr);

    if (IsSoftwareSysIntr(dwSysIntr))
        return;

    if (irq >= S5L_IRQ_MAX)
    {
        OALMSG(1, (L"OEMInterruptDisable: unknown SysIntr %d\r\n", dwSysIntr));
        return;
    }
    IrqDisable(irq);
}

// ---------------------------------------------------------------------------
// OEMInterruptDone: REQUIRED
//
// The ISR masks a device IRQ before signalling its IST; InterruptDone
// re-enables it. The tick is never masked.
//
void OEMInterruptDone(DWORD dwSysIntr)
{
    UINT32 irq;

    if (dwSysIntr == SYSINTR_RESCHED || IsSoftwareSysIntr(dwSysIntr))
        return;

    irq = SysIntrToIrq(dwSysIntr);
    if (irq >= S5L_IRQ_MAX)
    {
        OALMSG(1, (L"OEMInterruptDone: unknown SysIntr %d\r\n", dwSysIntr));
        return;
    }
    IrqEnable(irq);
}

// ---------------------------------------------------------------------------
// OEMInterruptMask: OPTIONAL
//
void OEMInterruptMask(DWORD dwSysIntr, BOOL fDisable)
{
    if (fDisable)
        OEMInterruptDisable(dwSysIntr);
    else
        OEMInterruptEnable(dwSysIntr, NULL, 0);
}

// ---------------------------------------------------------------------------
// OEMInterruptHandler: REQUIRED
//
ULONG OEMInterruptHandler(ULONG ra)
{
    UINT32 sysIntr = SYSINTR_NOP;
    UINT32 status0, status1;

    UNREFERENCED_PARAMETER(ra);

    // reading VICxADDRESS marks the interrupt as in service
    (void)VIC0_REG(VIC_ADDRESS);
    (void)VIC1_REG(VIC_ADDRESS);

    status0 = VIC0_REG(VIC_IRQSTATUS);
    status1 = VIC1_REG(VIC_IRQSTATUS);

    if (status0 & (1u << S5L_IRQ_TIMER))
    {
        // system tick (timer B)
        OALTimerTickIsr();

        if ((int)(CurMSec - dwReschedTime) >= 0)
            sysIntr = SYSINTR_RESCHED;
    }
    else if (status0 & (1u << S5L_IRQ_UART0))
    {
        // mask until OEMInterruptDone from the IST
        IrqDisable(S5L_IRQ_UART0);
        sysIntr = SYSINTR_UART0;
    }
    else
    {
        // unexpected source: mask it so it cannot storm
        if (status0 != 0)
            VIC0_REG(VIC_INTENCLEAR) = status0;
        if (status1 != 0)
            VIC1_REG(VIC_INTENCLEAR) = status1;
        OALMSG(1, (L"OEMInterruptHandler: unexpected IRQ status %08x %08x, masked\r\n",
                   status0, status1));
    }

    VIC0_REG(VIC_ADDRESS) = 0;
    VIC1_REG(VIC_ADDRESS) = 0;

    return sysIntr;
}

// ---------------------------------------------------------------------------
// OEMInterruptHandlerFIQ: REQUIRED (FIQ unused)
//
void OEMInterruptHandlerFIQ(void)
{
    return;
}
