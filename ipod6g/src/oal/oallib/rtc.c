//
// rtc.c - real time clock for the ipod6g BSP (stub)
//
// The iPod classic RTC lives in the PCF50635 PMU (I2C0, address 0xE6).
// Until there is an I2C driver the OAL reports a fixed time.
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>
#include <oal.h>

void InitRTC(void)
{
    return;
}

BOOL OEMGetRealTime(LPSYSTEMTIME lpst)
{
    if (lpst == NULL)
        return FALSE;

    lpst->wYear         = 2026;
    lpst->wMonth        = 1;
    lpst->wDay          = 1;
    lpst->wDayOfWeek    = 4;    // Thursday
    lpst->wHour         = 0;
    lpst->wMinute       = 0;
    lpst->wSecond       = 0;
    lpst->wMilliseconds = 0;
    return TRUE;
}

BOOL OEMSetRealTime(LPSYSTEMTIME lpst)
{
    UNREFERENCED_PARAMETER(lpst);
    return TRUE;
}

BOOL OEMSetAlarmTime(LPSYSTEMTIME lpst)
{
    UNREFERENCED_PARAMETER(lpst);
    return TRUE;
}
