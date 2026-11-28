//
// ipttouch.c - iPod touch 2G touch driver: the PDD of Microsoft's CE 7 touch
// stream driver (step 4b)
//
// tchstreammdd.lib (the MDD) is the stream driver "TCH1:" that device
// manager loads; GWES talks to it through tchproxy.dll, which finds it by
// the touch driver class in IClass (platform.reg). The MDD calls
// TchPdd_Init; this PDD starts a worker thread that loads the controller
// firmware and calibration from the image (\Windows\z2fw.bin,
// \Windows\z2cal.bin: Apple's, put into the image from the gitignored
// ipodtouch2g\files\z2\ by PreFmergeBib.bat), starts the Zephyr2 controller
// (z2.c), polls the data-ready line every 2 ms and reads frames.
//
// Each frame's first finger becomes one sample for the MDD (single touch):
// already in screen coordinates, in quarter pixels (tchproxy's subpixel
// accuracy 4), flagged TOUCHEVENTF_CALIBRATED so tchproxy does not run it
// through the calibration DLL. The mapping from the sensor surface (from the
// controller's dimensions report, 4602 x 7306) to the 320 x 480 screen was
// checked on 2026-11-28: x to the right, y downwards, no swap or flip.
//
// Structure of the PDD from Microsoft's SMDK6410 touch driver
// (SRC\DRIVERS\Touch\s3c6410_touch.cpp) and tchstreamddsi.h.
//
#include <windows.h>
#include <pm.h>
#include <tchstreamddsi.h>

#include "z2hw.h"
#include "z2.h"

#define FW_PATH             L"\\Windows\\z2fw.bin"
#define CAL_PATH            L"\\Windows\\z2cal.bin"

#define SCREEN_W            320
#define SCREEN_H            480
#define SUBPIXEL            4           // tchproxy SUBPIXEL_ACCURACY
#define POLL_MS             2
#define MAX_ERRORS_LOGGED   10
#define SAMPLE_RATE         60          // the controller scans at about 60 Hz

#define RESTART_AFTER_ERRORS    100     // bad frames in a row before a restart
#define MAX_RESTARTS            3

// Frame read speed: registry FrameHz in the driver's key. Linux reads at
// 2 MHz; here answers came one byte late at 2 MHz while a finger was down
// (2026-11-28), so the default is slower. Start-up traffic always uses
// 83 kHz (z2.c).
#define REG_KEY             L"Drivers\\BuiltIn\\Touch"
#define DEFAULT_FRAME_HZ    500000
#define MIN_FRAME_HZ        24000       // CLKDIV 0x3ff at 24 MHz: 23.5 kHz
#define MAX_FRAME_HZ        4500000

static HANDLE g_hThread = NULL;
static volatile BOOL g_run = FALSE;
static volatile BOOL g_enabled = TRUE;  // IOCTL_TOUCH_ENABLE/DISABLE_TOUCHPANEL
static Z2_STATE g_z2;
static DWORD g_frameHz = DEFAULT_FRAME_HZ;
static DWORD g_sampleRate = SAMPLE_RATE;
static CEDEVICE_POWER_STATE g_powerState = D0;

// the MDD's side
static PFN_TCH_MDD_REPORTSAMPLESET g_pfnReport = NULL;
static DWORD g_mddContext = 0;

//------------------------------------------------------------------------------
// Helpers
//
static DWORD ReadFrameHz(void)
{
    HKEY hKey;
    DWORD value = 0, size = sizeof(value), type = 0;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, REG_KEY, 0, 0, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueEx(hKey, L"FrameHz", NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS ||
            type != REG_DWORD)
            value = 0;
        RegCloseKey(hKey);
    }
    if (value == 0)
        return DEFAULT_FRAME_HZ;
    if (value < MIN_FRAME_HZ)
        return MIN_FRAME_HZ;
    if (value > MAX_FRAME_HZ)
        return MAX_FRAME_HZ;
    return value;
}

static BYTE *LoadFile(const WCHAR *pszPath, DWORD *pLen)
{
    HANDLE h;
    DWORD size, got = 0;
    BYTE *p;

    h = CreateFile(pszPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        RETAILMSG(1, (L"ipttouch: cannot open %s (error %d)\r\n", pszPath, GetLastError()));
        return NULL;
    }
    size = GetFileSize(h, NULL);
    p = (size != 0 && size != 0xFFFFFFFF) ? (BYTE *)LocalAlloc(LMEM_FIXED, size) : NULL;
    if (p == NULL || !ReadFile(h, p, size, &got, NULL) || got != size)
    {
        RETAILMSG(1, (L"ipttouch: cannot read %s (%d of %d bytes)\r\n", pszPath, got, size));
        if (p != NULL)
            LocalFree(p);
        CloseHandle(h);
        return NULL;
    }
    CloseHandle(h);
    *pLen = size;
    return p;
}

static void FormatBytes(const BYTE *p, int len, WCHAR *pszOut)
{
    static const WCHAR hex[] = L"0123456789abcdef";
    int i;

    for (i = 0; i < len; i++)
    {
        pszOut[i * 3]     = hex[p[i] >> 4];
        pszOut[i * 3 + 1] = hex[p[i] & 0xF];
        pszOut[i * 3 + 2] = L' ';
    }
    pszOut[(len > 0) ? (len * 3 - 1) : 0] = L'\0';
}

// Receive FIFO bytes the hardware layer threw away (see z2hw.c Segment)
static void LogFifoStats(const WCHAR *pszIndent)
{
    Z2HW_STATS st;

    Z2Hw_GetStats(&st);
    RETAILMSG(1, (L"ipttouch: %sreceive FIFO: %d stale bytes before transfers (first 0x%02x), %d extra after (first 0x%02x)\r\n",
                  pszIndent, st.staleBefore, st.firstStale, st.extraAfter, st.firstExtra));
}

// The details of the last bad frame (which check, the bytes)
static void LogBadFrame(void)
{
    WCHAR tx[16 * 3 + 1], rx[16 * 3 + 1];
    int n = (g_z2.errLen < 16) ? g_z2.errLen : 16;

    FormatBytes(g_z2.errTx, n, tx);
    FormatBytes(g_z2.errRx, n, rx);
    RETAILMSG(1, (L"ipttouch: bad frame %d (%d frames, %d empty): %s (value 0x%x), %d-byte transfer, nop %d, data ready %d\r\n",
                  g_z2.errors, g_z2.frames, g_z2.empty, Z2_ErrName(g_z2.errStep), g_z2.errValue,
                  g_z2.errLen, g_z2.curNop, Z2Hw_DataReady()));
    RETAILMSG(1, (L"ipttouch:   late answers completed so far %d\r\n", g_z2.realigns));
    RETAILMSG(1, (L"ipttouch:   sent     %s\r\n", tx));
    RETAILMSG(1, (L"ipttouch:   received %s\r\n", rx));
    LogFifoStats(L"  ");
}

// Sensor units -> screen quarter pixels, clamped to the screen
static void ToScreen(const Z2_FINGER *pF, LONG *pX, LONG *pY)
{
    LONG x = (LONG)pF->x * (SCREEN_W * SUBPIXEL) / g_z2.sensorWidth;
    LONG y = (LONG)pF->y * (SCREEN_H * SUBPIXEL) / g_z2.sensorHeight;

    if (x < 0) x = 0;
    if (x > SCREEN_W * SUBPIXEL - 1) x = SCREEN_W * SUBPIXEL - 1;
    if (y < 0) y = 0;
    if (y > SCREEN_H * SUBPIXEL - 1) y = SCREEN_H * SUBPIXEL - 1;
    *pX = x;
    *pY = y;
}

// One sample to the MDD (single touch: the primary contact only)
static void Report(DWORD flags, LONG x, LONG y)
{
    CETOUCHINPUT input;

    if (g_pfnReport == NULL || !g_enabled)
        return;
    memset(&input, 0, sizeof(input));
    input.x = x;
    input.y = y;
    input.dwID = 0;
    input.dwFlags = flags | TOUCHEVENTF_PRIMARY | TOUCHEVENTF_CALIBRATED;
    g_pfnReport(g_mddContext, 1, &input);
}

static BOOL StartController(const BYTE *pFw, DWORD fwLen, const BYTE *pCal, DWORD calLen)
{
    Z2_FINGER fingers[Z2_MAX_FINGERS];
    int n;

    if (!Z2_Boot(&g_z2, pFw, fwLen, pCal, calLen, g_frameHz))
    {
        RETAILMSG(1, (L"ipttouch: the controller did not start; held in reset\r\n"));
        return FALSE;
    }

    // as Linux's probe: read one frame so the controller starts reporting
    n = Z2_ReadFrame(&g_z2, fingers, Z2_MAX_FINGERS);
    if (n < 0)
        LogBadFrame();
    RETAILMSG(1, (L"ipttouch: controller running (first read %d), data ready %d\r\n",
                  n, Z2Hw_DataReady()));
    return TRUE;
}

//------------------------------------------------------------------------------
// Worker thread
//
static DWORD WINAPI TouchThread(LPVOID pContext)
{
    BYTE *pFw = NULL, *pCal = NULL;
    DWORD fwLen = 0, calLen = 0, errorsLogged = 0, lastStats;
    DWORD badInRow = 0, restarts = 0;
    Z2_FINGER fingers[Z2_MAX_FINGERS];
    BOOL fDown = FALSE;
    LONG x = 0, y = 0;
    int n;

    UNREFERENCED_PARAMETER(pContext);

    pFw = LoadFile(FW_PATH, &fwLen);
    pCal = LoadFile(CAL_PATH, &calLen);
    if (pFw == NULL || pCal == NULL)
        goto out;
    g_frameHz = ReadFrameHz();
    RETAILMSG(1, (L"ipttouch: firmware %d bytes, calibration %d bytes; frames read at %d Hz (registry FrameHz)\r\n",
                  fwLen, calLen, g_frameHz));

    if (!StartController(pFw, fwLen, pCal, calLen))
        goto out;

    lastStats = GetTickCount();
    while (g_run)
    {
        if (Z2Hw_DataReady() != 0)
        {
            Sleep(POLL_MS);
            continue;
        }

        n = Z2_ReadFrame(&g_z2, fingers, Z2_MAX_FINGERS);
        if (n < 0)
        {
            badInRow++;
            if (errorsLogged < MAX_ERRORS_LOGGED)
            {
                LogBadFrame();
                errorsLogged++;
            }
            else if ((g_z2.errors % 100) == 0)
            {
                RETAILMSG(1, (L"ipttouch: bad frame (%d errors, %d frames, %d empty)\r\n",
                              g_z2.errors, g_z2.frames, g_z2.empty));
            }

            if (badInRow >= RESTART_AFTER_ERRORS)
            {
                // lift a held contact first, so nothing stays pressed
                if (fDown)
                {
                    Report(TOUCHEVENTF_UP, x, y);
                    fDown = FALSE;
                }
                if (restarts >= MAX_RESTARTS)
                {
                    RETAILMSG(1, (L"ipttouch: %d bad frames in a row after %d restarts, giving up (held in reset)\r\n",
                                  badInRow, restarts));
                    Z2Hw_Reset(TRUE);
                    goto out;
                }
                restarts++;
                RETAILMSG(1, (L"ipttouch: %d bad frames in a row, restarting the controller (%d)\r\n",
                              badInRow, restarts));
                if (!StartController(pFw, fwLen, pCal, calLen))
                    goto out;
                badInRow = 0;
                errorsLogged = 0;       // the new start's first bad frames in detail again
                continue;
            }
            Sleep(3);                   // let the controller settle (as Linux)
            continue;
        }
        badInRow = 0;

        if (n > 0)
        {
            ToScreen(&fingers[0], &x, &y);
            if (!fDown)
            {
                Report(TOUCHEVENTF_DOWN | TOUCHEVENTF_INRANGE, x, y);
                fDown = TRUE;
                RETAILMSG(1, (L"ipttouch: down at %d,%d (sensor %d,%d)\r\n",
                              x / SUBPIXEL, y / SUBPIXEL, fingers[0].x, fingers[0].y));
            }
            else
            {
                Report(TOUCHEVENTF_MOVE | TOUCHEVENTF_INRANGE, x, y);
            }
        }
        else if (fDown)
        {
            // up at the last position
            Report(TOUCHEVENTF_UP, x, y);
            fDown = FALSE;
            RETAILMSG(1, (L"ipttouch: up at %d,%d\r\n", x / SUBPIXEL, y / SUBPIXEL));
        }

        if (GetTickCount() - lastStats >= 60000)
        {
            RETAILMSG(1, (L"ipttouch: %d frames, %d empty, %d errors, %d late answers completed (frames at %d Hz)\r\n",
                          g_z2.frames, g_z2.empty, g_z2.errors, g_z2.realigns, g_z2.frameHz));
            lastStats = GetTickCount();
        }
    }

out:
    if (fDown)
        Report(TOUCHEVENTF_UP, x, y);
    if (pFw != NULL)
        LocalFree(pFw);
    if (pCal != NULL)
        LocalFree(pCal);
    return 0;
}

//------------------------------------------------------------------------------
// PDD interface (tchstreamddsi.h)
//
static void WINAPI TchPdd_Deinit(DWORD hPddContext)
{
    UNREFERENCED_PARAMETER(hPddContext);

    if (g_hThread != NULL)
    {
        g_run = FALSE;
        WaitForSingleObject(g_hThread, 5000);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    Z2Hw_Reset(TRUE);
    g_pfnReport = NULL;
}

static void WINAPI TchPdd_PowerDown(DWORD hPddContext)
{
    UNREFERENCED_PARAMETER(hPddContext);
}

static void WINAPI TchPdd_PowerUp(DWORD hPddContext)
{
    UNREFERENCED_PARAMETER(hPddContext);
}

static BOOL WINAPI TchPdd_Ioctl(DWORD hPddContext, DWORD dwCode, PBYTE pBufIn, DWORD dwLenIn,
                                PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    DWORD dwResult = ERROR_INVALID_PARAMETER;

    UNREFERENCED_PARAMETER(hPddContext);

    switch (dwCode)
    {
    case IOCTL_TOUCH_ENABLE_TOUCHPANEL:
        g_enabled = TRUE;
        dwResult = ERROR_SUCCESS;
        break;

    case IOCTL_TOUCH_DISABLE_TOUCHPANEL:
        g_enabled = FALSE;
        dwResult = ERROR_SUCCESS;
        break;

    case IOCTL_TOUCH_GET_SAMPLE_RATE:
        if (pBufOut != NULL && dwLenOut == sizeof(DWORD))
        {
            __try
            {
                *(DWORD *)pBufOut = g_sampleRate;
                if (pdwActualOut != NULL)
                    *pdwActualOut = sizeof(DWORD);
                dwResult = ERROR_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_TOUCH_SET_SAMPLE_RATE:
        // the controller sets its own rate; accept and remember the value
        if (pBufIn != NULL && dwLenIn == sizeof(DWORD))
        {
            __try
            {
                g_sampleRate = *(DWORD *)pBufIn;
                dwResult = ERROR_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_TOUCH_GET_TOUCH_PROPS:
        if (pBufOut != NULL && dwLenOut == sizeof(TCH_PROPS))
        {
            __try
            {
                TCH_PROPS *pProps = (TCH_PROPS *)pBufOut;

                memset(pProps, 0, sizeof(*pProps));
                pProps->minSampleRate          = SAMPLE_RATE;
                pProps->maxSampleRate          = SAMPLE_RATE;
                pProps->minCalCount            = TCH_CALIBMODE_DEFAULT_MINCALCOUNT;
                pProps->calHoldSteadyTime      = TCH_CALIBMODE_DEFAULT_CALHOLDSTEADYTIME;
                pProps->calDeltaReset          = TCH_CALIBMODE_DEFAULT_CALDELTARESET;
                pProps->xRangeMin              = 0;
                pProps->xRangeMax              = SCREEN_W * SUBPIXEL - 1;
                pProps->yRangeMin              = 0;
                pProps->yRangeMax              = SCREEN_H * SUBPIXEL - 1;
                pProps->touchType              = TOUCHTYPE_SINGLETOUCH;
                pProps->dwTouchInputMask       = 0;
                pProps->maxSimultaneousSamples = 1;
                pProps->cbPropertyMax          = 0;
                if (pdwActualOut != NULL)
                    *pdwActualOut = sizeof(TCH_PROPS);
                dwResult = ERROR_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_POWER_CAPABILITIES:
        if (pBufOut != NULL && dwLenOut == sizeof(POWER_CAPABILITIES))
        {
            __try
            {
                PPOWER_CAPABILITIES ppc = (PPOWER_CAPABILITIES)pBufOut;

                memset(ppc, 0, sizeof(*ppc));
                ppc->DeviceDx = DX_MASK(D0) | DX_MASK(D1) | DX_MASK(D2) | DX_MASK(D3) | DX_MASK(D4);
                if (pdwActualOut != NULL)
                    *pdwActualOut = sizeof(POWER_CAPABILITIES);
                dwResult = ERROR_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_POWER_GET:
        if (pBufOut != NULL && dwLenOut == sizeof(CEDEVICE_POWER_STATE))
        {
            __try
            {
                *(PCEDEVICE_POWER_STATE)pBufOut = g_powerState;
                if (pdwActualOut != NULL)
                    *pdwActualOut = sizeof(CEDEVICE_POWER_STATE);
                dwResult = ERROR_SUCCESS;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    case IOCTL_POWER_SET:
        // no power saving yet (the controller keeps scanning); remember the state
        if (pBufOut != NULL && dwLenOut == sizeof(CEDEVICE_POWER_STATE))
        {
            __try
            {
                CEDEVICE_POWER_STATE dx = *(PCEDEVICE_POWER_STATE)pBufOut;

                if (VALID_DX(dx))
                {
                    g_powerState = dx;
                    if (pdwActualOut != NULL)
                        *pdwActualOut = sizeof(CEDEVICE_POWER_STATE);
                    dwResult = ERROR_SUCCESS;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                dwResult = ERROR_INVALID_PARAMETER;
            }
        }
        break;

    default:
        // includes IOCTL_TOUCH_GET_CALIBRATION_POINTS: tchproxy's defaults
        dwResult = ERROR_NOT_SUPPORTED;
        break;
    }

    if (dwResult != ERROR_SUCCESS)
    {
        SetLastError(dwResult);
        return FALSE;
    }
    return TRUE;
}

DWORD WINAPI TchPdd_Init(LPCTSTR pszActiveKey, TCH_MDD_INTERFACE_INFO *pMddIfc,
                         TCH_PDD_INTERFACE_INFO *pPddIfc, DWORD hMddContext)
{
    UNREFERENCED_PARAMETER(pszActiveKey);

    RETAILMSG(1, (L"ipttouch: TchPdd_Init\r\n"));
    if (pMddIfc == NULL || pPddIfc == NULL)
        return 0;

    g_pfnReport = pMddIfc->pfnMddReportSampleSet;
    g_mddContext = hMddContext;

    pPddIfc->version      = 1;
    pPddIfc->pfnDeinit    = TchPdd_Deinit;
    pPddIfc->pfnIoctl     = TchPdd_Ioctl;
    pPddIfc->pfnPowerDown = TchPdd_PowerDown;
    pPddIfc->pfnPowerUp   = TchPdd_PowerUp;

    if (!Z2Hw_Init())
        return 1;                       // stay loaded; the log says what is wrong

    g_run = TRUE;
    g_hThread = CreateThread(NULL, 0, TouchThread, NULL, 0, NULL);
    if (g_hThread == NULL)
    {
        g_run = FALSE;
        RETAILMSG(1, (L"ipttouch: cannot create the thread\r\n"));
    }
    return 1;                           // PDD context (unused)
}
