//
// ipodi2c.c - the iPod's I2C bus 0 under Windows CE: stream driver "I2C1:"
//
// The one owner of I2C controller 0. The power chip (clock, charger,
// battery measurement, supplies) and the audio codec share this bus; every
// driver that talks to either goes through here, so transfers never overlap.
//
// Interface: Microsoft's (public\common\oak\inc\i2c.h, the same entry points,
// IOCTLs and semantics as its I2C driver in public\common\oak\drivers\i2c):
//   h = I2cOpen(L"I2C1:");  I2cSetSlaveAddress(h, IPODI2C_ADDR_PMU);
//   I2cRead(h, reg, buf, n);  I2cWrite(h, reg, buf, n);  I2cClose(h);
// The register number goes through SetFilePointer (I2C_Seek), the transfer
// through ReadFile / WriteFile; each returns the bytes moved, or -1 with
// GetLastError set. As in Microsoft's driver, only callers in the device
// manager's process (kernel-mode drivers) may open it.
// Supported: 7-bit chip addresses, 1-byte register numbers. Speed and
// timeout settings are accepted and kept but not used: the bus runs at the
// setting Rockbox uses, and every wait is bounded by a loop count (i2chw.c).
//
// Not Microsoft's: which chips and registers may be reached is fixed in
// g_allow below. Anything else is refused before the bus is touched
// (ERROR_ACCESS_DENIED), so a wrong caller cannot write the power chip's
// supply settings. IOCTL_IPODI2C_GET_STATE returns the driver's account.
//
#include <windows.h>
#include <winioctl.h>
#include <pkfuncs.h>
#include <i2c.h>

#include <ipodi2c.h>
#include "i2chw.h"

#define LOG_FAILURES        16          // failures and refusals written to the debug output

// What may be reached. Reads: any register of a listed chip. Writes: only
// registers wFirst .. wLast (a write covers reg .. reg + count - 1; the chips
// count the register number up by themselves).
typedef struct {
    BYTE    chip;
    BYTE    wFirst;
    BYTE    wLast;
    LPCWSTR name;
} I2C_ALLOW;

static const I2C_ALLOW g_allow[] = {
    // PCF50635: ADCC3, ADCC2, ADCC1 only, the battery measurement Rockbox
    // starts (pmu-6g.c pmu_read_adc). Its supply and charger settings stay
    // out of reach.
    { IPODI2C_ADDR_PMU,   0x52, 0x54, L"power chip" },
    // CS42L55: all of it (Rockbox cs42l55.c writes 0x00 .. 0x3F).
    { IPODI2C_ADDR_CODEC, 0x00, 0xFF, L"codec" },
};

// one per I2cOpen
typedef struct {
    DWORD   chip;
    DWORD   reg;
    DWORD   speed;
    DWORD   timeout;
} I2C_OPEN;

static CRITICAL_SECTION g_cs;           // one transfer at a time, and g_st
static IPODI2C_STATE g_st;
static BOOL g_init = FALSE;

static const I2C_ALLOW *FindChip(DWORD chip)
{
    DWORD i;

    for (i = 0; i < sizeof(g_allow) / sizeof(g_allow[0]); i++)
    {
        if (g_allow[i].chip == chip)
            return &g_allow[i];
    }
    return NULL;
}

static DWORD Win32Error(DWORD code)
{
    switch (code)
    {
    case IPODI2C_E_NOT_MAPPED:
    case IPODI2C_E_PINS:        return ERROR_NOT_READY;
    case IPODI2C_E_NOT_READY:
    case IPODI2C_E_NO_END:      return ERROR_TIMEOUT;
    case IPODI2C_E_NO_ANSWER:   return ERROR_IO_DEVICE;
    case IPODI2C_E_REFUSED:     return ERROR_ACCESS_DENIED;
    default:                    return ERROR_INVALID_PARAMETER;
    }
}

// Notes a failed or refused transfer (g_cs held). Returns the Win32 error.
static DWORD Account(BOOL fWrite, DWORD chip, DWORD reg, const I2CHW_FAIL *pFail)
{
    g_st.lastStep = pFail->step;
    g_st.lastCode = pFail->code;
    g_st.lastStat = pFail->stat;
    g_st.lastSta2 = pFail->sta2;
    g_st.lastChip = chip;
    g_st.lastReg = reg;
    if (pFail->code == IPODI2C_E_REFUSED)
        g_st.refused++;
    else
        g_st.errors++;
    if (g_st.errors + g_st.refused <= LOG_FAILURES)
    {
        RETAILMSG(1, (L"ipodi2c: %s chip 0x%02x reg 0x%02x failed: step %u code %u (IICSTAT %08x IICSTA2 %08x)\r\n",
                      fWrite ? L"write" : L"read", chip, reg, pFail->step, pFail->code,
                      pFail->stat, pFail->sta2));
    }
    return Win32Error(pFail->code);
}

// One read or write for an open handle. Returns the bytes moved or -1.
static DWORD Transfer(I2C_OPEN *pOpen, BOOL fWrite, BYTE *pUser, DWORD count)
{
    BYTE buf[IPODI2C_MAX_LENGTH];
    const I2C_ALLOW *pAllow;
    I2CHW_FAIL fail;
    DWORD chip, reg, rc, err;

    if (pOpen == NULL || pUser == NULL || count == 0 || count > IPODI2C_MAX_LENGTH)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (DWORD)-1;
    }
    chip = pOpen->chip;
    reg = pOpen->reg;
    if (fWrite)
    {
        __try
        {
            memcpy(buf, pUser, count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return (DWORD)-1;
        }
    }

    EnterCriticalSection(&g_cs);
    memset(&fail, 0, sizeof(fail));
    pAllow = FindChip(chip);
    if (reg > 0xFF || pAllow == NULL
        || (fWrite && (reg < pAllow->wFirst || reg + count - 1 > pAllow->wLast)))
    {
        fail.step = IPODI2C_STEP_CHECK;
        fail.code = IPODI2C_E_REFUSED;
        err = Account(fWrite, chip, reg, &fail);
        LeaveCriticalSection(&g_cs);
        SetLastError(err);
        return (DWORD)-1;
    }
    if (fWrite)
        rc = I2cHw_Write((BYTE)chip, (BYTE)reg, buf, count, &fail);
    else
        rc = I2cHw_Read((BYTE)chip, (BYTE)reg, buf, count, &fail);
    if (rc != IPODI2C_OK)
    {
        err = Account(fWrite, chip, reg, &fail);
        LeaveCriticalSection(&g_cs);
        SetLastError(err);
        return (DWORD)-1;
    }
    g_st.lastStep = 0;
    g_st.lastCode = IPODI2C_OK;
    if (fWrite)
        g_st.writes++;
    else
        g_st.reads++;
    LeaveCriticalSection(&g_cs);

    if (!fWrite)
    {
        __try
        {
            memcpy(pUser, buf, count);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return (DWORD)-1;
        }
    }
    return count;
}

//------------------------------------------------------------------------------
// stream interface

DWORD I2C_Init(LPCTSTR pContext, LPVOID lpvBusContext)
{
    UNREFERENCED_PARAMETER(pContext);
    UNREFERENCED_PARAMETER(lpvBusContext);

    if (g_init)
        return 0;               // one bus, one instance
    InitializeCriticalSection(&g_cs);
    memset(&g_st, 0, sizeof(g_st));
    g_st.version = IPODI2C_VERSION;
    if (!I2cHw_Map(&g_st.bootPcon3, &g_st.bootPwrcon1))
    {
        RETAILMSG(1, (L"ipodi2c: cannot map the registers\r\n"));
        DeleteCriticalSection(&g_cs);
        return 0;
    }
    g_st.mapped = 1;
    g_init = TRUE;
    RETAILMSG(1, (L"ipodi2c: I2C1: = bus 0, bus pins %08x (%s), clock gates %08x\r\n",
                  g_st.bootPcon3,
                  ((g_st.bootPcon3 & 0xFF0) == 0x220) ? L"set up" : L"NOT set up",
                  g_st.bootPwrcon1));
    return 1;
}

BOOL I2C_Deinit(DWORD dwContext)
{
    UNREFERENCED_PARAMETER(dwContext);
    return FALSE;               // stays loaded: other drivers depend on it
}

DWORD I2C_Open(DWORD dwContext, DWORD dwAccess, DWORD dwShareMode)
{
    I2C_OPEN *pOpen;

    UNREFERENCED_PARAMETER(dwContext);
    UNREFERENCED_PARAMETER(dwAccess);
    UNREFERENCED_PARAMETER(dwShareMode);

    // as Microsoft's driver: kernel-mode drivers only
    if (GetCurrentProcessId() != (DWORD)GetDirectCallerProcessId())
    {
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    pOpen = (I2C_OPEN *)LocalAlloc(LPTR, sizeof(I2C_OPEN));
    if (pOpen == NULL)
    {
        SetLastError(ERROR_OUTOFMEMORY);
        return 0;
    }
    pOpen->chip = 0xFFFFFFFF;   // none until I2cSetSlaveAddress
    pOpen->speed = I2C_STANDARD_SPEED;
    pOpen->timeout = I2C_DEFAULT_TIMEOUT;
    return (DWORD)pOpen;
}

BOOL I2C_Close(DWORD hOpenContext)
{
    if (hOpenContext != 0)
        LocalFree((HLOCAL)hOpenContext);
    return TRUE;
}

DWORD I2C_Read(DWORD hOpenContext, LPVOID pBuffer, DWORD Count)
{
    return Transfer((I2C_OPEN *)hOpenContext, FALSE, (BYTE *)pBuffer, Count);
}

DWORD I2C_Write(DWORD hOpenContext, LPCVOID pBuffer, DWORD Count)
{
    return Transfer((I2C_OPEN *)hOpenContext, TRUE, (BYTE *)pBuffer, Count);
}

// The register number (Microsoft's I2cRead / I2cWrite: SetFilePointer, FILE_BEGIN).
DWORD I2C_Seek(DWORD hOpenContext, long Amount, WORD Type)
{
    I2C_OPEN *pOpen = (I2C_OPEN *)hOpenContext;

    UNREFERENCED_PARAMETER(Type);
    if (pOpen == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return (DWORD)-1;
    }
    pOpen->reg = (DWORD)Amount;
    return (DWORD)Amount;
}

static BOOL InDword(PBYTE pIn, DWORD lenIn, DWORD *pValue)
{
    if (pIn == NULL || lenIn != sizeof(DWORD))
        return FALSE;
    __try
    {
        *pValue = *(DWORD *)pIn;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return FALSE;
    }
    return TRUE;
}

BOOL I2C_IOControl(DWORD hOpenContext, DWORD dwCode, PBYTE pBufIn, DWORD dwLenIn,
                   PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    I2C_OPEN *pOpen = (I2C_OPEN *)hOpenContext;
    IPODI2C_STATE st;
    DWORD v = 0, err = ERROR_SUCCESS;

    if (pOpen == NULL)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (dwCode)
    {
    case IOCTL_I2C_SET_SLAVE_ADDRESS:
        if (!InDword(pBufIn, dwLenIn, &v) || v > 0x7F)
            err = ERROR_INVALID_PARAMETER;
        else
            pOpen->chip = v;
        break;

    case IOCTL_I2C_SET_SLAVE_ADDR_SIZE:     // 7 bits only
        if (!InDword(pBufIn, dwLenIn, &v))
            err = ERROR_INVALID_PARAMETER;
        else if (v != 7)
            err = ERROR_NOT_SUPPORTED;
        break;

    case IOCTL_I2C_SET_REG_ADDR_SIZE:       // 1 byte only
        if (!InDword(pBufIn, dwLenIn, &v))
            err = ERROR_INVALID_PARAMETER;
        else if (v != 1)
            err = ERROR_NOT_SUPPORTED;
        break;

    case IOCTL_I2C_SET_SPEED:               // kept, not used (see the top of this file)
        if (!InDword(pBufIn, dwLenIn, &v))
        {
            err = ERROR_INVALID_PARAMETER;
            break;
        }
        pOpen->speed = (v > I2C_STANDARD_SPEED) ? I2C_FAST_SPEED : I2C_STANDARD_SPEED;
        if (pBufOut != NULL && dwLenOut == sizeof(DWORD))
        {
            __try
            {
                *(DWORD *)pBufOut = pOpen->speed;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                err = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_I2C_SET_TIMEOUT:             // kept, not used
        if (!InDword(pBufIn, dwLenIn, &v) || v == 0 || v > I2C_MAX_TIMEOUT)
            err = ERROR_INVALID_PARAMETER;
        else
            pOpen->timeout = v;
        break;

    case IOCTL_IPODI2C_GET_STATE:
        if (pBufOut == NULL || dwLenOut < sizeof(st))
        {
            err = ERROR_INVALID_PARAMETER;
            break;
        }
        EnterCriticalSection(&g_cs);
        st = g_st;
        LeaveCriticalSection(&g_cs);
        __try
        {
            memcpy(pBufOut, &st, sizeof(st));
            if (pdwActualOut != NULL)
                *pdwActualOut = sizeof(st);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            err = ERROR_INVALID_PARAMETER;
        }
        break;

    default:
        err = ERROR_NOT_SUPPORTED;
        break;
    }

    if (err != ERROR_SUCCESS)
    {
        SetLastError(err);
        return FALSE;
    }
    return TRUE;
}

BOOL WINAPI DllMain(HANDLE hInstance, DWORD dwReason, LPVOID pReserved)
{
    UNREFERENCED_PARAMETER(pReserved);
    if (dwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls((HMODULE)hInstance);
    return TRUE;
}
