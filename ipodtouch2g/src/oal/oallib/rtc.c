//
// rtc.c - the clock of the ipodtouch2g BSP: counted from the kernel's tick
//
// The iPod touch's real clock presumably lives in its power chip (openiBoot
// plat-s5l8720/pmu.c), and the kernel layer does not talk to that chip. It
// keeps the time itself instead: a start value plus the milliseconds the
// tick has counted since. The start value is 1 January 2026 00:00 until Windows sets
// the time (SetLocalTime ends here in OEMSetRealTime); a driver that can
// read the power chip does that once Windows runs.
//
// The kernel calls these one at a time.
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>
#include <oal.h>

static UINT64 g_baseMs = 0;         // milliseconds since 1 January 1601 at g_baseTick
static DWORD  g_baseTick = 0;       // CurMSec at that moment
static BOOL   g_haveBase = FALSE;

static BOOL SetBase(const SYSTEMTIME *pst)
{
    ULARGE_INTEGER t;
    FILETIME ft;

    if (!NKSystemTimeToFileTime(pst, &ft))
        return FALSE;
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    g_baseMs = t.QuadPart / 10000;
    g_baseTick = CurMSec;
    g_haveBase = TRUE;
    return TRUE;
}

void InitRTC(void)
{
    return;
}

BOOL OEMGetRealTime(LPSYSTEMTIME lpst)
{
    SYSTEMTIME start;
    ULARGE_INTEGER t;
    FILETIME ft;
    DWORD tick, delta;

    if (lpst == NULL)
        return FALSE;

    if (!g_haveBase)
    {
        memset(&start, 0, sizeof(start));
        start.wYear = 2026;
        start.wMonth = 1;
        start.wDay = 1;
        start.wDayOfWeek = 4;       // Thursday
        if (!SetBase(&start))
            return FALSE;
    }

    tick = CurMSec;
    delta = tick - g_baseTick;
    // the 32-bit difference would wrap after 49 days: move the start along now and then
    if (delta > 0x40000000)
    {
        g_baseMs += delta;
        g_baseTick = tick;
        delta = 0;
    }

    t.QuadPart = (g_baseMs + delta) * 10000;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    return NKFileTimeToSystemTime(&ft, lpst);
}

BOOL OEMSetRealTime(LPSYSTEMTIME lpst)
{
    if (lpst == NULL)
        return FALSE;
    return SetBase(lpst);
}

BOOL OEMSetAlarmTime(LPSYSTEMTIME lpst)
{
    UNREFERENCED_PARAMETER(lpst);
    return TRUE;
}
