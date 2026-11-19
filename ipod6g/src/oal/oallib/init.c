//
// init.c - OEMInit for the ipod6g BSP (iPod classic, S5L8702, ARM926EJ-S)
//
// Structure follows the CEonPi raspi2 BSP; ARMv5 specifics follow the
// Microsoft 3DS_iMX27 (ARM926) BSP.
//
#include <windows.h>
#include <winnt.h>
#include <pkfuncs.h>
#include <oemglobal.h>

#include <bsp.h>

//------------------------------------------------------------------------------
// Prototypes
//
void SetOALGlobalVariables(void);
void InitRTC(void);
void Debug(LPCTSTR szFormat, ...);
BOOL OALIsProcessorFeaturePresent(DWORD feature);

//------------------------------------------------------------------------------
// Externs
//

// Kernel security cookie (for linking)
extern DWORD_PTR __security_cookie;
extern DWORD_PTR __security_cookie_complement;

// Kernel page pool parameters
extern NKPagePoolParameters pagePoolParams;

// ---------------------------------------------------------------------------
// OEMInit: REQUIRED
//
// Called by the kernel after OEMInitDebugSerial, with interrupts disabled.
//
void OEMInit(void)
{
    OEMWriteDebugString(L"\r\nOEMInit++\r\n");

    SetOALGlobalVariables();

    // ARM926EJ-S: ARMv5TEJ, no VFP (values as in the 3DS_iMX27 BSP)
    CEProcessorType  = PROCESSOR_STRONGARM;
    CEInstructionSet = PROCESSOR_ARM_V5_INSTRUCTION;

    // Memory for DrWatson kernel support
    dwNKDrWatsonSize = 128 * 1024;

    // Processor feature query
    g_pOemGlobal->pfnIsProcessorFeaturePresent =
        (PFN_IsProcessorFeaturePresent)OALIsProcessorFeaturePresent;

    // RTC (stub until the PCF50635 PMU driver exists)
    InitRTC();

    // Cache globals (oal_cache_arm926 reads the CP15 cache type register)
    OALCacheGlobalsInit();

    Debug(L"DCache: %d sets, %d ways, %d line, %d bytes\r\n",
          g_oalCacheInfo.L1DSetsPerWay, g_oalCacheInfo.L1DNumWays,
          g_oalCacheInfo.L1DLineSize, g_oalCacheInfo.L1DSize);
    Debug(L"ICache: %d sets, %d ways, %d line, %d bytes\r\n",
          g_oalCacheInfo.L1ISetsPerWay, g_oalCacheInfo.L1INumWays,
          g_oalCacheInfo.L1ILineSize, g_oalCacheInfo.L1ISize);

    // No persistent storage yet: always clean boot
    NKForceCleanBoot();

    // Interrupt controllers
    if (!OALIntrInit())
    {
        OALMSG(OAL_ERROR, (L"[OAL:ERR] OEMInit: failed to initialize interrupts\r\n"));
    }

    // System tick (timer B, 1 ms) and performance counter (timer E, 1 MHz)
    OALTimerInit(RESCHED_PERIOD, OEM_COUNT_1MS, 0);

    Debug(L"Main memory end: 0x%08x\r\n", g_pOemGlobal->dwMainMemoryEndAddress);

    // Start KITL (no-op if the image has no kitl.dll)
    KITLIoctl(IOCTL_KITL_STARTUP, NULL, 0, NULL, 0, NULL);

    OEMWriteDebugString(L"OEMInit--\r\n");
    return;
}

// ---------------------------------------------------------------------------
// SetOALGlobalVariables: CUSTOM
//
// Overrides the defaults of the kernel's OAL global variables.
//
void SetOALGlobalVariables(void)
{
    g_pOemGlobal->dwDefaultThreadQuantum = DEFAULT_THREAD_QUANTUM;

    // No coprocessor (VFP) state to save on the ARM926
    g_pOemGlobal->cbCoProcRegSize = 0;
    g_pOemGlobal->fSaveCoProcReg = FALSE;

    // No hardware watchdog refresh yet
    g_pOemGlobal->dwWatchDogPeriod = 0;
    g_pOemGlobal->dwWatchDogThreadPriority = DEFAULT_WATCHDOG_PRIORITY;

    g_pOemGlobal->cbErrReportSize = 0;
    g_pOemGlobal->pROMChain = NULL;
    g_pOemGlobal->pKitlInfo = NULL;

    // /GS security cookies
    g_pOemGlobal->p__security_cookie = &__security_cookie;
    g_pOemGlobal->p__security_cookie_complement = &__security_cookie_complement;

    g_pOemGlobal->dwAlarmResolution = DEFAULT_ALARMRESOLUTION_MSEC;
    g_pOemGlobal->dwYearsRTCRollover = 0;

    // No VFP
    g_pOemGlobal->dwPlatformFlags = 0;

    // Page pool sizing for 64 MB of RAM
    g_pOemGlobal->pPagePoolParam = &pagePoolParams;
    pagePoolParams.Loader.Target = 4 * 1024 * 1024;
    pagePoolParams.File.Target = 2 * 1024 * 1024;
    g_pOemGlobal->cpPageOutLow = 512 * 1024 / VM_PAGE_SIZE;
    g_pOemGlobal->cpPageOutHigh = 2 * 1024 * 1024 / VM_PAGE_SIZE;

    g_pOemGlobal->fMPEnable = FALSE;

#ifdef DEBUG
    g_pOemGlobal->pdpCurSettings = &dpCurSettings;
#else
    g_pOemGlobal->pdpCurSettings = NULL;
#endif

    return;
}
