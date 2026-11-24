//
// ipodbatt.c - the iPod's battery and clock for Windows CE
//
// The hardware half of battdrvr.dll; the other half is Microsoft's battery
// driver library (battdrvr_mdd.lib), which asks this file for the battery
// status every few seconds and serves Windows' power status functions.
//
// Both the battery measurement and the clock live in the iPod's power chip,
// reached through ipodi2c.c (the power chip's bus layer):
//   - at start-up the chip's clock is read once and given to Windows
//     (SetLocalTime); Windows' clock then runs on the kernel's tick. The
//     chip's clock is never set from here.
//   - the battery voltage is measured the way Rockbox does it (the bus layer's
//     one fixed write), at most every 20 seconds, and turned into a
//     percentage with Rockbox's table for this iPod.
//   - "external power" is the chip's charger status, "charging" the
//     processor pin Rockbox uses for it.
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <battimpl.h>

#include <ipodi2c.h>
#include <ipodbatt.h>

#define PA_GPIO             0x3CF00000
#define GPIO_PDAT11         0x164       // bit 4 clear = charging

#define REG_MBCS1           0x4B        // bit 0: USB power there, bit 2: adapter there
#define REG_ADCS1           0x55
#define REG_ADCS3           0x57
#define REG_RTCSC           0x59        // .. 0x5F: second, minute, hour, weekday, day, month, year (BCD)

#define MEASURE_EVERY_MS    20000
#define MEASURE_POLLS       50
#define MEASURE_POLL_MS     2

// Windows' three battery states. "Low" brings a taskbar icon, "critical" a
// warning box: neither should come while there is plenty left.
#define PERCENT_LOW         20
#define PERCENT_CRITICAL    5

static CRITICAL_SECTION g_cs;
static volatile UINT32 *g_pGpio = NULL;
static BOOL  g_haveMv = FALSE, g_haveCharger = FALSE;
static DWORD g_mv = 0, g_lastMeasureTick = 0;
static BYTE  g_mbcs1 = 0;
static IPODBATT_STATUS g_st;        // what this driver did, for the power panel

// Rockbox's table for this iPod: millivolts at 0, 10, ... 100 percent
static const WORD g_levels[11] = { 3500, 3670, 3720, 3750, 3770, 3800, 3860, 3920, 3980, 4070, 4170 };

static DWORD Percent(DWORD mv)
{
    int i;

    if (mv <= g_levels[0])
        return 0;
    for (i = 1; i < 11; i++)
    {
        if (mv < g_levels[i])
            return (i - 1) * 10 + (mv - g_levels[i - 1]) * 10 / (g_levels[i] - g_levels[i - 1]);
    }
    return 100;
}

static BOOL Bcd(BYTE b, int max, WORD *pOut)
{
    int v = (b >> 4) * 10 + (b & 0x0F);

    if ((b & 0x0F) > 9 || (b >> 4) > 9 || v > max)
        return FALSE;
    *pOut = (WORD)v;
    return TRUE;
}

// The power chip's time, once, into Windows' clock.
static void SetClockFromChip(void)
{
    SYSTEMTIME st;
    BYTE r[7];
    WORD year;

    if (IpodI2c_Read(IPODI2C_PMU, REG_RTCSC, r, sizeof(r)) != IPODI2C_OK)
    {
        RETAILMSG(1, (L"ipodbatt: the power chip's clock could not be read\r\n"));
        return;
    }
    g_st.clockRead = 1;
    memcpy(g_st.clockRaw, r, sizeof(r));
    memset(&st, 0, sizeof(st));
    if (!Bcd(r[0], 59, &st.wSecond) || !Bcd(r[1], 59, &st.wMinute) || !Bcd(r[2], 23, &st.wHour)
        || !Bcd(r[4], 31, &st.wDay) || !Bcd(r[5], 12, &st.wMonth) || !Bcd(r[6], 99, &year)
        || st.wDay == 0 || st.wMonth == 0)
    {
        RETAILMSG(1, (L"ipodbatt: the power chip's clock is not a date: %02x %02x %02x %02x %02x %02x %02x\r\n",
                      r[0], r[1], r[2], r[3], r[4], r[5], r[6]));
        return;
    }
    g_st.clockValid = 1;
    st.wYear = (WORD)(2000 + year);
    st.wDayOfWeek = (WORD)(r[3] & 7);
    if (st.wDayOfWeek > 6)
        st.wDayOfWeek = 0;
    if (SetLocalTime(&st))
    {
        g_st.timeAccepted = 1;
        RETAILMSG(1, (L"ipodbatt: clock set to %u-%02u-%02u %02u:%02u:%02u from the power chip\r\n",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond));
    }
    else
    {
        g_st.timeError = GetLastError();
        RETAILMSG(1, (L"ipodbatt: Windows refused the time (error %u)\r\n", g_st.timeError));
    }
}

// The power panel asks what this driver did (Microsoft's half passes on the
// requests it does not know). Returns a Win32 error code.
static DWORD OwnIoControl(DWORD dwContext, DWORD Ioctl, PUCHAR pInBuf, DWORD InBufLen,
                          PUCHAR pOutBuf, DWORD OutBufLen, PDWORD pdwBytesTransferred)
{
    IPODBATT_STATUS st;

    UNREFERENCED_PARAMETER(dwContext);
    UNREFERENCED_PARAMETER(pInBuf);
    UNREFERENCED_PARAMETER(InBufLen);

    if (Ioctl != IOCTL_IPODBATT_GET_STATUS)
        return ERROR_NOT_SUPPORTED;
    if (pOutBuf == NULL || OutBufLen < sizeof(st))
        return ERROR_INVALID_PARAMETER;
    EnterCriticalSection(&g_cs);
    IpodI2c_GetState(&g_st.bus);
    g_st.charging = (g_pGpio != NULL) && ((g_pGpio[GPIO_PDAT11 >> 2] & 0x10) == 0);
    st = g_st;
    LeaveCriticalSection(&g_cs);
    __try
    {
        memcpy(pOutBuf, &st, sizeof(st));
        if (pdwBytesTransferred != NULL)
            *pdwBytesTransferred = sizeof(st);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return ERROR_INVALID_PARAMETER;
    }
    return ERROR_SUCCESS;
}

// One battery measurement, as Rockbox does it: start, wait for "ready", read.
static BOOL MeasureMv(DWORD *pMv)
{
    BYTE s3 = 0, s1 = 0;
    DWORD polls, raw;

    if (IpodI2c_StartBatteryMeasurement() != IPODI2C_OK)
        return FALSE;
    for (polls = 0; polls < MEASURE_POLLS; polls++)
    {
        if (IpodI2c_Read(IPODI2C_PMU, REG_ADCS3, &s3, 1) != IPODI2C_OK)
            return FALSE;
        if (s3 & 0x80)
            break;
        Sleep(MEASURE_POLL_MS);
    }
    if (!(s3 & 0x80))
        return FALSE;
    if (IpodI2c_Read(IPODI2C_PMU, REG_ADCS1, &s1, 1) != IPODI2C_OK)
        return FALSE;
    raw = ((DWORD)s1 << 2) | (s3 & 3);
    *pMv = PMUTEST_MV(raw);
    return TRUE;
}

BOOL WINAPI BatteryPDDInitialize(LPCTSTR pszRegistryContext)
{
    PHYSICAL_ADDRESS a;

    UNREFERENCED_PARAMETER(pszRegistryContext);

    InitializeCriticalSection(&g_cs);
    memset(&g_st, 0, sizeof(g_st));
    g_st.version = IPODBATT_VERSION;
    a.QuadPart = PA_GPIO;
    g_pGpio = (volatile UINT32 *)MmMapIoSpace(a, 0x1000, FALSE);
    IpodI2c_Init();
    gpfnBatteryPddIOControl = OwnIoControl;

    EnterCriticalSection(&g_cs);
    SetClockFromChip();
    LeaveCriticalSection(&g_cs);
    return TRUE;                // without a readable chip the status is "unknown", not a failure
}

void WINAPI BatteryPDDDeinitialize(void)
{
}

void WINAPI BatteryPDDResume(void)
{
}

void WINAPI BatteryPDDPowerHandler(BOOL bOff)
{
    UNREFERENCED_PARAMETER(bOff);
}

BOOL WINAPI BatteryPDDGetStatus(PSYSTEM_POWER_STATUS_EX2 pstatus, PBOOL pfBatteriesChangedSinceLastCall)
{
    DWORD now = GetTickCount(), mv = 0, percent;
    BOOL haveMv, charging, external;
    BYTE m = 0, flag;

    EnterCriticalSection(&g_cs);
    g_st.asked++;
    if (!g_haveMv || now - g_lastMeasureTick >= MEASURE_EVERY_MS)
    {
        g_lastMeasureTick = now;
        if (MeasureMv(&mv))
        {
            if (!g_haveMv)
                RETAILMSG(1, (L"ipodbatt: battery %u mV, about %u %%\r\n", mv, Percent(mv)));
            g_mv = mv;
            g_haveMv = TRUE;
            g_st.measures++;
            g_st.mv = mv;
            g_st.percent = Percent(mv);
        }
        else
        {
            if (g_st.fails == 0)
                RETAILMSG(1, (L"ipodbatt: the battery measurement failed\r\n"));
            g_st.fails++;
        }
        if (IpodI2c_Read(IPODI2C_PMU, REG_MBCS1, &m, 1) == IPODI2C_OK)
        {
            g_mbcs1 = m;
            g_haveCharger = TRUE;
            g_st.mbcs1 = m;
        }
    }
    haveMv = g_haveMv;
    mv = g_mv;
    charging = (g_pGpio != NULL) && ((g_pGpio[GPIO_PDAT11 >> 2] & 0x10) == 0);
    external = g_haveCharger && (g_mbcs1 & 0x05) != 0;
    LeaveCriticalSection(&g_cs);

    memset(pstatus, 0, sizeof(*pstatus));
    pstatus->ACLineStatus = (external || charging) ? AC_LINE_ONLINE : AC_LINE_OFFLINE;
    if (haveMv)
    {
        percent = Percent(mv);
        flag = (percent > PERCENT_LOW) ? BATTERY_FLAG_HIGH
             : (percent > PERCENT_CRITICAL) ? BATTERY_FLAG_LOW : BATTERY_FLAG_CRITICAL;
        if (charging)
            flag |= BATTERY_FLAG_CHARGING;
        pstatus->BatteryFlag = flag;
        pstatus->BatteryLifePercent = (BYTE)percent;
        pstatus->BatteryVoltage = mv;
    }
    else
    {
        pstatus->BatteryFlag = BATTERY_FLAG_UNKNOWN;
        pstatus->BatteryLifePercent = BATTERY_PERCENTAGE_UNKNOWN;
    }
    pstatus->BatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BatteryChemistry = BATTERY_CHEMISTRY_LION;
    // there is no backup battery; "high, level unknown" is what Microsoft's own
    // sample reports for one that needs no attention
    pstatus->BackupBatteryFlag = BATTERY_FLAG_HIGH;
    pstatus->BackupBatteryLifePercent = BATTERY_PERCENTAGE_UNKNOWN;
    pstatus->BackupBatteryLifeTime = BATTERY_LIFE_UNKNOWN;
    pstatus->BackupBatteryFullLifeTime = BATTERY_LIFE_UNKNOWN;

    if (pfBatteriesChangedSinceLastCall != NULL)
        *pfBatteriesChangedSinceLastCall = FALSE;
    return TRUE;
}

// three levels for the main battery (high, low, critical), none for a backup battery
LONG BatteryPDDGetLevels(void)
{
    return MAKELONG(3, 0);
}

BOOL BatteryPDDSupportsChangeNotification(void)
{
    return FALSE;
}
