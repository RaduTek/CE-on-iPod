//
// init.c - OEMInit for the ipodtouch2g BSP (iPod touch 2G, S5L8720, ARM1176JZF-S)
//
// Structure follows the ipod6g BSP; ARMv6 specifics (processor type, VFP)
// follow the CEonPi raspi2 BSP, which runs CE 7 on the same core.
//
#include <windows.h>
#include <winnt.h>
#include <pkfuncs.h>
#include <oemglobal.h>
#include <vfpsupport.h>

#include <bsp.h>

//------------------------------------------------------------------------------
// Prototypes
//
void SetOALGlobalVariables(void);
void InitRTC(void);
void Debug(LPCTSTR szFormat, ...);
BOOL OALIsProcessorFeaturePresent(DWORD feature);
UINT32 OALTimerTicksPerMs(void);
void TouchProbe(void);          // touchprobe.c (empty without BSP_TOUCHPROBE)

// startup.s
BOOL StartupVfpPresent(void);
UINT32 StartupReadCpuId(void);

//------------------------------------------------------------------------------
// Externs
//

// Kernel security cookie (for linking)
extern DWORD_PTR __security_cookie;
extern DWORD_PTR __security_cookie_complement;

// Kernel page pool parameters
extern NKPagePoolParameters pagePoolParams;

#ifdef BSP_SCREEN_MARKS
// ---------------------------------------------------------------------------
// ScreenMark: fill display window 0 with one colour (as startup.s does),
// through the uncached DRAM view. Only touches the reserved top of DRAM.
//
static void ScreenMark(UINT32 rgb888, UINT16 rgb565)
{
    UINT32 format = (S5L_REG(S5L_CLCD_PA + CLCD_W0_FORMAT) >> 8) & 0x0F;
    UINT32 pa = S5L_REG(S5L_CLCD_PA + CLCD_W0_ADDR);
    UINT32 stride = S5L_REG(S5L_CLCD_PA + CLCD_W0_STRIDE);
    UINT32 height = S5L_REG(S5L_CLCD_PA + CLCD_W0_SIZE) & 0xFFFF;
    UINT32 words, value, i;
    volatile UINT32 *p;

    if (format == CLCD_FORMAT_RGB888)
    {
        words = stride * height;
        value = rgb888;
    }
    else if (format == CLCD_FORMAT_RGB565)
    {
        words = (stride * height) / 2;
        value = ((UINT32)rgb565 << 16) | rgb565;
    }
    else
    {
        return;
    }

    if (stride == 0 || stride > 0x800 || height == 0 || height > 0x800 || (pa & 3) != 0)
        return;
    if (pa < S5L_FB_RESERVED_PA ||
        pa + words * 4 > S5L_FB_RESERVED_PA + S5L_FB_RESERVED_SIZE)
        return;

    p = (volatile UINT32 *)S5L_DRAM_UA(pa);
    for (i = 0; i < words; i++)
        p[i] = value;
}
#endif

// ---------------------------------------------------------------------------
// OEMInit: REQUIRED
//
// Called by the kernel after OEMInitDebugSerial, with interrupts disabled.
//
void OEMInit(void)
{
    BOOL fVfp;

    OEMWriteDebugString(L"\r\nOEMInit++\r\n");

#ifdef BSP_SCREEN_MARKS
    // white: the kernel runs with the MMU on and has reached the OAL
    ScreenMark(0x00FFFFFF, 0xFFFF);
#endif

    SetOALGlobalVariables();

    // ARM1176JZF-S: ARMv6 (values as in the CEonPi BSP)
    CEProcessorType  = PROCESSOR_ARM11;
    CEInstructionSet = PROCESSOR_ARM_V6FP_INSTRUCTION;

    Debug(L"CPU ID 0x%08x\r\n", StartupReadCpuId());

    // Memory for DrWatson kernel support
    dwNKDrWatsonSize = 128 * 1024;

    // Processor feature query
    g_pOemGlobal->pfnIsProcessorFeaturePresent =
        (PFN_IsProcessorFeaturePresent)OALIsProcessorFeaturePresent;

    // VFP: hardware if the coprocessor answers, otherwise full emulation, so
    // code built for ARMV6 with floating point still runs
    fVfp = StartupVfpPresent();
    VfpOemInit(g_pOemGlobal, fVfp ? VFP_AUTO_DETECT_FPSID : VFP_FULL_EMULATION_FPSID);
    Debug(L"VFP: %s\r\n", fVfp ? L"present" : L"not found, emulated");

    // RTC (counted from the tick until a driver reads the power chip)
    InitRTC();

    // Cache globals (oal_cache_arm1136 reads the CP15 cache type register)
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

    // System tick (timer 4, 1 ms) and performance counter (64-bit tick counter)
    OALTimerInit(RESCHED_PERIOD, OALTimerTicksPerMs(), 0);

    Debug(L"Main memory end: 0x%08x\r\n", g_pOemGlobal->dwMainMemoryEndAddress);

    // Start KITL (no-op if the image has no kitl.dll)
    KITLIoctl(IOCTL_KITL_STARTUP, NULL, 0, NULL, 0, NULL);

    // Touch bring-up step 1: how iBoot left GPIO, SPI4 and the power chip
    TouchProbe();

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

    // VfpOemInit sets up the VFP context handling
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

    g_pOemGlobal->dwPlatformFlags = 0;

    // Page pool sizing for 128 MB of RAM
    g_pOemGlobal->pPagePoolParam = &pagePoolParams;
    pagePoolParams.Loader.Target = 8 * 1024 * 1024;
    pagePoolParams.File.Target = 4 * 1024 * 1024;
    g_pOemGlobal->cpPageOutLow = 1 * 1024 * 1024 / VM_PAGE_SIZE;
    g_pOemGlobal->cpPageOutHigh = 4 * 1024 * 1024 / VM_PAGE_SIZE;

    g_pOemGlobal->fMPEnable = FALSE;

#ifdef DEBUG
    g_pOemGlobal->pdpCurSettings = &dpCurSettings;
#else
    g_pOemGlobal->pdpCurSettings = NULL;
#endif

    return;
}
