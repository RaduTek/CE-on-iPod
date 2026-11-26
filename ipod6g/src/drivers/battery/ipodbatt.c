//
// ipodbatt.c - the iPod's battery and clock for Windows CE
//
// The hardware half of battdrvr.dll; the other half is Microsoft's battery
// driver library (battdrvr_mdd.lib), which asks this file for the battery
// status every few seconds and serves Windows' power status functions.
//
// Both the battery measurement and the clock live in the iPod's power chip,
// reached through the I2C driver "I2C1:" (src\drivers\ipodi2c) with
// Microsoft's I2C functions (i2c.h):
//   - once the bus can be opened (normally at start-up) the chip's clock is
//     read once and given to Windows (SetLocalTime); Windows' clock then runs
//     on the kernel's tick. The chip's clock is never set from here.
//   - the battery voltage is measured the way Rockbox does it (three fixed
//     bytes to registers 0x52 .. 0x54, the only writes this driver makes), at
//     most every 20 seconds, and turned into a percentage with Rockbox's
//     table for this iPod.
//   - "external power" is the chip's charger status, "charging" the
//     processor pin Rockbox uses for it.
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <battimpl.h>
#include <i2c.h>

#include <ipodi2c.h>
#include <ipodbatt.h>

#define PA_GPIO             0x3CF00000
#define GPIO_PDAT11         0x164       // bit 4 clear = charging

#define REG_MBCS1           0x4B        // bit 0: USB power there, bit 2: adapter there
#define REG_ADCC3           0x52        // battery measurement settings, 0x52 .. 0x54
#define REG_ADCC2           0x53
#define REG_ADCS1           0x55
#define REG_ADCS3           0x57
#define REG_RTCSC           0x59        // .. 0x5F: second, minute, hour, weekday, day, month, year (BCD)

// the battery measurement in Rockbox's terms: raw is 10 bits,
// millivolts = raw * 2000 / 1023 + 2250
#define RAW_TO_MV(raw)      ((raw) * 2000 / 1023 + 2250)

// The battery measurement of the PCF5063x, as Rockbox starts it (pmu-6g.c
// pmu_read_adc with the ADC_BATTERY channel of adc-6g.c):
//   register 0x52 (ADCC3) = 0x00   no divider, no accessory switch
//   register 0x53 (ADCC2) = 0x00   no ratio settings
//   register 0x54 (ADCC1) = 0x15   battery minus 2.25 V, average of 4, 10 bits, start
// The second write covers 0x53 and 0x54 in one transfer (the register
// number counts up by itself).
static const BYTE g_measure1[] = { 0x00 };          // at 0x52
static const BYTE g_measure2[] = { 0x00, 0x15 };    // at 0x53

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
static HANDLE g_hBus = INVALID_HANDLE_VALUE;    // "I2C1:", set to the power chip
static BOOL  g_clockTried = FALSE;

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

// The bus, opened on first use (g_cs held). The I2C driver loads first
// (registry "Order"); if it is not there yet, the next call tries again.
static BOOL OpenBus(void)
{
    HANDLE h;

    if (g_hBus != INVALID_HANDLE_VALUE)
        return TRUE;
    h = I2cOpen(IPODI2C_DEVICE);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (g_st.busOpenFails++ == 0)
            RETAILMSG(1, (L"ipodbatt: cannot open %s (error %u)\r\n", IPODI2C_DEVICE, GetLastError()));
        return FALSE;
    }
    if (!I2cSetSlaveAddress(h, IPODI2C_ADDR_PMU))
    {
        RETAILMSG(1, (L"ipodbatt: %s refused the power chip's address (error %u)\r\n",
                      IPODI2C_DEVICE, GetLastError()));
        I2cClose(h);
        g_st.busOpenFails++;
        return FALSE;
    }
    g_hBus = h;
    return TRUE;
}

static BOOL PmuRead(BYTE reg, BYTE *pOut, DWORD count)
{
    return OpenBus() && I2cRead(g_hBus, reg, pOut, count);
}

static BOOL PmuWrite(BYTE reg, const BYTE *pData, DWORD count)
{
    return OpenBus() && I2cWrite(g_hBus, reg, (LPBYTE)pData, count);
}

// The power chip's time, once, into Windows' clock.
static void SetClockFromChip(void)
{
    SYSTEMTIME st;
    BYTE r[7];
    WORD year;

    if (!PmuRead(REG_RTCSC, r, sizeof(r)))
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
    if (g_hBus != INVALID_HANDLE_VALUE)
        DeviceIoControl(g_hBus, IOCTL_IPODI2C_GET_STATE, NULL, 0, &g_st.bus, sizeof(g_st.bus), NULL, NULL);
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

    if (!PmuWrite(REG_ADCC3, g_measure1, sizeof(g_measure1))
        || !PmuWrite(REG_ADCC2, g_measure2, sizeof(g_measure2)))
        return FALSE;
    for (polls = 0; polls < MEASURE_POLLS; polls++)
    {
        if (!PmuRead(REG_ADCS3, &s3, 1))
            return FALSE;
        if (s3 & 0x80)
            break;
        Sleep(MEASURE_POLL_MS);
    }
    if (!(s3 & 0x80))
        return FALSE;
    if (!PmuRead(REG_ADCS1, &s1, 1))
        return FALSE;
    raw = ((DWORD)s1 << 2) | (s3 & 3);
    *pMv = RAW_TO_MV(raw);
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
    gpfnBatteryPddIOControl = OwnIoControl;

    EnterCriticalSection(&g_cs);
    if (OpenBus())
    {
        g_clockTried = TRUE;
        SetClockFromChip();
    }
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
    if (!g_clockTried && OpenBus())     // the bus was not there at start-up
    {
        g_clockTried = TRUE;
        SetClockFromChip();
    }
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
        if (PmuRead(REG_MBCS1, &m, 1))
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
