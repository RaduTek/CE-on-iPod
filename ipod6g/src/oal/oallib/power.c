//
// power.c - power management OAL functions for the ipod6g BSP (stubs)
//
// OEMIdle returns immediately (busy idle) for now. Later: ARM926
// wait-for-interrupt (mcr p15, 0, r0, c7, c0, 4) and PMU-based power off.
//
#include <windows.h>
#include <oemglobal.h>

void OEMIdle(DWORD dwIdleParam)
{
    UNREFERENCED_PARAMETER(dwIdleParam);
    return;
}

void OEMPowerOff(void)
{
    return;
}

void OEMHaltSystem(void)
{
    return;
}

void OEMIdleEx(LARGE_INTEGER *pliIdleTime)
{
    UNREFERENCED_PARAMETER(pliIdleTime);
    return;
}
