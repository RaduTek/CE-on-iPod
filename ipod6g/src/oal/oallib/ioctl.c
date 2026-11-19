//
// ioctl.c - OEMIoControl for the ipod6g BSP
//
// Small hand-written IOCTL table, as in the CEonPi BSP. KITL's
// OEMKitlIoctl (oal_kitl.lib) forwards anything it does not handle here.
//
#include <windows.h>
#include <oemglobal.h>
#include <oal.h>
#include <nkintr.h>

#include <bsp.h>

//------------------------------------------------------------------------------
// Prototypes
//
BOOL OALIoCtlHalRequestSysIntr(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                               VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);
BOOL OALIoCtlHalInitRegistry(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                             VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);
BOOL OALIoCtlHalReboot(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                       VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);
static BOOL IpodIoCtlHalGetDeviceInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                                      VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize);

//------------------------------------------------------------------------------
// IOCTL table
//
static struct {
    BOOL postInit;
    CRITICAL_SECTION cs;
} g_ioctlState = { FALSE };

const OAL_IOCTL_HANDLER g_ipodIoCtlTable[] =
{
    { IOCTL_HAL_REQUEST_SYSINTR,    0,  OALIoCtlHalRequestSysIntr   },
    { IOCTL_HAL_INITREGISTRY,       0,  OALIoCtlHalInitRegistry     },
    { IOCTL_HAL_REBOOT,             0,  OALIoCtlHalReboot           },
    { IOCTL_HAL_GET_DEVICE_INFO,    0,  IpodIoCtlHalGetDeviceInfo   },
    { 0,                            0,  NULL                        }
};

__declspec(noinline) static BOOL DoOEMIoControlWithCS(
    DWORD dwIndex, DWORD code, VOID *pInBuffer, DWORD inSize,
    VOID *pOutBuffer, DWORD outSize, DWORD *pOutSize)
{
    BOOL rc = FALSE;

    EnterCriticalSection(&g_ioctlState.cs);
    __try
    {
        rc = g_ipodIoCtlTable[dwIndex].pfnHandler(
            code, pInBuffer, inSize, pOutBuffer, outSize, (UINT32 *)pOutSize);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        rc = FALSE;
        NKSetLastError(ERROR_INVALID_PARAMETER);
    }
    LeaveCriticalSection(&g_ioctlState.cs);
    return rc;
}

// ---------------------------------------------------------------------------
// OEMIoControl: REQUIRED
//
BOOL OEMIoControl(DWORD dwIoControlCode, LPVOID lpInBuf, DWORD nInBufSize,
                  LPVOID lpOutBuf, DWORD nOutBufSize, LPDWORD lpBytesReturned)
{
    BOOL rc = FALSE;
    UINT32 dwIndex;

    OALMSG(OAL_IOCTL && OAL_FUNC, (L"+OEMIoControl(0x%x)\r\n", dwIoControlCode));

    if (!g_ioctlState.postInit && dwIoControlCode == IOCTL_HAL_POSTINIT)
    {
        InitializeCriticalSection(&g_ioctlState.cs);
        g_ioctlState.postInit = TRUE;
    }

    for (dwIndex = 0; g_ipodIoCtlTable[dwIndex].pfnHandler != NULL; dwIndex++)
    {
        if (g_ipodIoCtlTable[dwIndex].code == dwIoControlCode)
            break;
    }

    if (g_ipodIoCtlTable[dwIndex].pfnHandler == NULL)
    {
        NKSetLastError(ERROR_NOT_SUPPORTED);
        OALMSG(OAL_IOCTL, (L"OEMIoControl: unsupported code 0x%x (device 0x%04x func %d)\r\n",
                           dwIoControlCode, dwIoControlCode >> 16, (dwIoControlCode >> 2) & 0x0FFF));
        return FALSE;
    }

    if (g_ioctlState.postInit && (g_ipodIoCtlTable[dwIndex].flags & OAL_IOCTL_FLAG_NOCS) == 0)
    {
        rc = DoOEMIoControlWithCS(dwIndex, dwIoControlCode, lpInBuf, nInBufSize,
                                  lpOutBuf, nOutBufSize, lpBytesReturned);
    }
    else
    {
        rc = g_ipodIoCtlTable[dwIndex].pfnHandler(dwIoControlCode, lpInBuf, nInBufSize,
                                                  lpOutBuf, nOutBufSize, (UINT32 *)lpBytesReturned);
    }

    OALMSG(OAL_IOCTL && OAL_FUNC, (L"-OEMIoControl(rc = %d)\r\n", rc));
    return rc;
}

// ---------------------------------------------------------------------------
// OEMKDIoctl: OPTIONAL
//
BOOL OEMKDIoctl(DWORD dwCode, LPVOID pBuf, DWORD cbSize)
{
    UNREFERENCED_PARAMETER(dwCode);
    UNREFERENCED_PARAMETER(pBuf);
    UNREFERENCED_PARAMETER(cbSize);
    return TRUE;
}

// ---------------------------------------------------------------------------
// OALIsProcessorFeaturePresent: ARM926EJ-S (ARMv5TEJ, no VFP, VIVT caches)
//
BOOL OALIsProcessorFeaturePresent(DWORD feature)
{
    switch (feature)
    {
    case PF_ARM_V4:
    case PF_ARM_V5:
    case PF_ARM_THUMB:
    case PF_ARM_DSP:
    case PF_ARM_JAZELLE:
    case PF_ARM_MOVE_CP:
    case PF_ARM_WRITE_BUFFER:
    case PF_ARM_WRITE_BACK_CACHE:
    case PF_ARM_CACHE_CAN_BE_LOCKED_DOWN:
        return TRUE;

    default:
        // PF_ARM_V6/V7, VFP, NEON, Thumb-2, L2 cache, unaligned access,
        // physically tagged cache (ARM926 is VIVT), TCMs (not used), ...
        return FALSE;
    }
}

// ---------------------------------------------------------------------------
// OALIoCtlHalRequestSysIntr: static IRQ -> SYSINTR mapping
//
// Input is either a single IRQ or the {-1, flags, irq} triplet.
//
BOOL OALIoCtlHalRequestSysIntr(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                               VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    DWORD *pIn = (DWORD *)pInpBuffer;
    DWORD dwIrq;
    DWORD dwSysIntr = SYSINTR_UNDEFINED;

    UNREFERENCED_PARAMETER(code);

    if ((pInpBuffer == NULL) || (inpSize < sizeof(DWORD)) ||
        (pOutBuffer == NULL) || (outSize < sizeof(DWORD)))
    {
        NKSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if ((pIn[0] == 0xFFFFFFFF) && (inpSize >= 3 * sizeof(DWORD)))
        dwIrq = pIn[2];
    else
        dwIrq = pIn[0];

    if (dwIrq == S5L_IRQ_UART0)
        dwSysIntr = SYSINTR_UART0;

    if (dwSysIntr == SYSINTR_UNDEFINED)
    {
        OALMSG(OAL_WARN, (L"OALIoCtlHalRequestSysIntr: no SYSINTR for IRQ %d\r\n", dwIrq));
        NKSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    *(DWORD *)pOutBuffer = dwSysIntr;
    if (pOutSize != NULL)
        *pOutSize = sizeof(DWORD);
    return TRUE;
}

// ---------------------------------------------------------------------------
// OALIoCtlHalInitRegistry: nothing to add yet
//
BOOL OALIoCtlHalInitRegistry(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                             VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UNREFERENCED_PARAMETER(code);
    UNREFERENCED_PARAMETER(pInpBuffer);
    UNREFERENCED_PARAMETER(inpSize);
    UNREFERENCED_PARAMETER(pOutBuffer);
    UNREFERENCED_PARAMETER(outSize);
    UNREFERENCED_PARAMETER(pOutSize);
    return TRUE;
}

// ---------------------------------------------------------------------------
// OALIoCtlHalReboot: SoC reset through the watchdog (Rockbox system_reboot)
//
// After the reset the iPod goes through the boot ROM and the Rockbox
// bootloader again, which loads rockbox.ipod from the disk.
//
BOOL OALIoCtlHalReboot(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                       VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    UNREFERENCED_PARAMETER(code);
    UNREFERENCED_PARAMETER(pInpBuffer);
    UNREFERENCED_PARAMETER(inpSize);
    UNREFERENCED_PARAMETER(pOutBuffer);
    UNREFERENCED_PARAMETER(outSize);
    UNREFERENCED_PARAMETER(pOutSize);

    OALMSG(1, (L"OALIoCtlHalReboot: resetting through the watchdog\r\n"));

    S5L_REG(S5L_WDTCON_PA) = S5L_WDTCON_RESET;
    for (;;)
        ;
}

// ---------------------------------------------------------------------------
// IpodIoCtlHalGetDeviceInfo: platform strings for IOCTL_HAL_GET_DEVICE_INFO
//
// Input is the SPI_* code (DWORD); output is a NUL-terminated WCHAR string.
// Only the platform identity codes are answered; everything else reports
// ERROR_NOT_SUPPORTED, as before this handler existed.
//
static BOOL IpodIoCtlHalGetDeviceInfo(UINT32 code, VOID *pInpBuffer, UINT32 inpSize,
                                      VOID *pOutBuffer, UINT32 outSize, UINT32 *pOutSize)
{
    LPCWSTR pszValue = NULL;
    UINT32 cbValue;

    UNREFERENCED_PARAMETER(code);

    if ((pInpBuffer == NULL) || (inpSize < sizeof(DWORD)))
    {
        NKSetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    switch (*(DWORD *)pInpBuffer)
    {
    case SPI_GETPLATFORMNAME:
    case SPI_GETPLATFORMTYPE:           // deprecated alias of the platform name
        pszValue = IOCTL_PLATFORM_NAME;
        break;
    case SPI_GETPLATFORMMANUFACTURER:
        pszValue = IOCTL_PLATFORM_MANUFACTURER;
        break;
    case SPI_GETOEMINFO:
        pszValue = IOCTL_PLATFORM_OEM;
        break;
    default:
        NKSetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    cbValue = (NKwcslen(pszValue) + 1) * sizeof(WCHAR);
    if (pOutSize != NULL)
        *pOutSize = cbValue;

    if ((pOutBuffer == NULL) || (outSize < cbValue))
    {
        NKSetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    memcpy(pOutBuffer, pszValue, cbValue);
    return TRUE;
}
