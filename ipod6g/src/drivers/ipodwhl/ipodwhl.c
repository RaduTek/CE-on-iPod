//
// ipodwhl.c - the iPod's click wheel under Windows CE
//
// Stream driver "WHL1:", kernel mode. Rockbox leaves the wheel's controller
// running when it hands over to Windows CE; this driver reads it the way
// Rockbox itself does (Rockbox, INT_WHEEL and ipod_4g_button_read in
// firmware/target/arm/ipod/button-clickwheel.c), only by looking every few
// milliseconds where Rockbox waits for the interrupt: read the flags, write the
// set ones back to clear them, read the report. That flag write is the only
// thing this driver writes anywhere. A controller whose clock is switched
// off (Rockbox does that while the hold switch is on) is left alone: reading it
// could hang the machine.
//
// Reports become events in whlcore.c (a button pressed or let go, the wheel
// one step on, 24 steps a turn). Reading the device hands them out;
// whlkeys.exe turns them into keys. Nothing is set up here: the controller's
// own start-up is Rockbox's.
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <ipodwhl.h>

#define PA_CLK              0x3C500000
#define CLK_PWRCON1         0x4C
#define PWRCON1_CWHEEL      (1u << 1)       // 1 = the wheel controller has no clock

#define PA_WHEEL            0x3C200000
#define WHEELINT            0x14            // flags; writing a 1 clears that flag
#define WHEELRX             0x18            // the report

#define PA_GPIO             0x3CF00000
#define GPIO_PCON14         0x1C0

#define LOOK_PRIORITY       150
#define RING                64
#define READ_WAIT_MS        100

static CRITICAL_SECTION g_cs;
static IPODWHL_STATUS g_st;
static volatile UINT32 *g_pClk, *g_pWheel, *g_pGpio;
static HANDLE g_hThread, g_hEvents;         // g_hEvents: set while the ring holds events
static volatile LONG g_run;
static WHL_EVENT g_ring[RING];
static DWORD g_ringHead, g_ringCount;
static WHL_INPUT g_in;

static volatile UINT32 *Map(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return (volatile UINT32 *)MmMapIoSpace(a, 0x1000, FALSE);
}

static DWORD WINAPI LookThread(LPVOID arg)
{
    WHL_EVENT ev[WHL_MAX_EVENTS];
    DWORD last = GetTickCount(), now, pwr, flags, rx, lastRx = 0;
    BOOL first = TRUE, haveRx = FALSE;
    int n, i;

    UNREFERENCED_PARAMETER(arg);
    CeSetThreadPriority(GetCurrentThread(), LOOK_PRIORITY);
    while (g_run)
    {
        now = GetTickCount();
        EnterCriticalSection(&g_cs);
        if (!first && now - last > g_st.maxGapMs)
            g_st.maxGapMs = now - last;
        first = FALSE;
        last = now;

        pwr = g_pClk[CLK_PWRCON1 >> 2];
        g_st.pwrcon1 = pwr;
        g_st.pcon14 = g_pGpio[GPIO_PCON14 >> 2];
        if (pwr & PWRCON1_CWHEEL)
        {
            g_st.gateClosed++;              // no clock: its registers are not to be touched
            LeaveCriticalSection(&g_cs);
            Sleep(50);
            continue;
        }
        g_st.looks++;
        flags = g_pWheel[WHEELINT >> 2];
        if (flags & 7)
        {
            // as Rockbox's interrupt routine: each flag that is set, written back by itself
            g_st.flagsSeen |= flags;
            if (flags & 4)
                g_pWheel[WHEELINT >> 2] = 4;
            if (flags & 2)
                g_pWheel[WHEELINT >> 2] = 2;
            if (flags & 1)
                g_pWheel[WHEELINT >> 2] = 1;
            g_st.cleared++;
        }
        rx = g_pWheel[WHEELRX >> 2];
        if (!haveRx || rx != lastRx)
        {
            haveRx = TRUE;
            lastRx = rx;
            g_st.reports++;
            g_st.lastRx = rx;
            n = Whl_Feed(&g_in, rx, now, ev);
            for (i = 0; i < n; i++)
            {
                if (g_ringCount >= RING)
                {
                    // nobody fetches: the oldest goes
                    g_ringHead = (g_ringHead + 1) % RING;
                    g_ringCount--;
                    g_st.dropped++;
                }
                g_ring[(g_ringHead + g_ringCount) % RING] = ev[i];
                g_ringCount++;
                g_st.events++;
            }
            if (n > 0)
                SetEvent(g_hEvents);
            g_st.buttons = g_in.buttons;
            g_st.touched = (DWORD)g_in.touched;
            g_st.pos = (DWORD)g_in.pos;
        }
        LeaveCriticalSection(&g_cs);
        Sleep(1);
    }
    return 0;
}

DWORD WHL_Init(LPCTSTR pContext, DWORD dwBusContext)
{
    UNREFERENCED_PARAMETER(pContext);
    UNREFERENCED_PARAMETER(dwBusContext);

    InitializeCriticalSection(&g_cs);
    memset(&g_st, 0, sizeof(g_st));
    memset(&g_in, 0, sizeof(g_in));
    g_st.version = IPODWHL_VERSION;
    g_hEvents = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_pClk = Map(PA_CLK);
    g_pWheel = Map(PA_WHEEL);
    g_pGpio = Map(PA_GPIO);
    g_st.mapped = (g_pClk != NULL && g_pWheel != NULL && g_pGpio != NULL && g_hEvents != NULL);
    if (g_st.mapped)
    {
        g_run = 1;
        g_hThread = CreateThread(NULL, 0, LookThread, NULL, 0, NULL);
        if (g_hThread == NULL)
            g_run = 0;
        g_st.running = (g_hThread != NULL);
    }
    return 1;                   // stay loaded in any case, so the panel can say what is wrong
}

BOOL WHL_Deinit(DWORD hDeviceContext)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
    if (g_hThread != NULL)
    {
        g_run = 0;
        WaitForSingleObject(g_hThread, 2000);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    return TRUE;
}

DWORD WHL_Open(DWORD hDeviceContext, DWORD AccessCode, DWORD ShareMode)
{
    UNREFERENCED_PARAMETER(AccessCode);
    UNREFERENCED_PARAMETER(ShareMode);
    return hDeviceContext;
}

BOOL WHL_Close(DWORD hOpenContext)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    return TRUE;
}

// Whole WHL_EVENT records; waits up to READ_WAIT_MS for the first one.
DWORD WHL_Read(DWORD hOpenContext, LPVOID pBuffer, DWORD Count)
{
    WHL_EVENT *pOut = (WHL_EVENT *)pBuffer;
    DWORD done = 0;

    UNREFERENCED_PARAMETER(hOpenContext);
    if (pBuffer == NULL || Count < sizeof(WHL_EVENT) || g_hEvents == NULL)
        return 0;

    WaitForSingleObject(g_hEvents, READ_WAIT_MS);

    EnterCriticalSection(&g_cs);
    __try
    {
        while (g_ringCount != 0 && done + sizeof(WHL_EVENT) <= Count)
        {
            *pOut++ = g_ring[g_ringHead];
            g_ringHead = (g_ringHead + 1) % RING;
            g_ringCount--;
            done += sizeof(WHL_EVENT);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        done = 0;
    }
    if (g_ringCount == 0)
        ResetEvent(g_hEvents);
    LeaveCriticalSection(&g_cs);
    return done;
}

DWORD WHL_Write(DWORD hOpenContext, LPCVOID pBuffer, DWORD Count)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(Count);
    return (DWORD)-1;
}

DWORD WHL_Seek(DWORD hOpenContext, long Amount, WORD Type)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(Amount);
    UNREFERENCED_PARAMETER(Type);
    return (DWORD)-1;
}

BOOL WHL_IOControl(DWORD hOpenContext, DWORD dwCode, PBYTE pBufIn, DWORD dwLenIn,
                   PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    IPODWHL_STATUS st;
    BOOL rc = FALSE;

    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBufIn);
    UNREFERENCED_PARAMETER(dwLenIn);

    if (dwCode == IOCTL_IPODWHL_GET_STATUS && pBufOut != NULL && dwLenOut >= sizeof(IPODWHL_STATUS))
    {
        EnterCriticalSection(&g_cs);
        st = g_st;
        LeaveCriticalSection(&g_cs);
        __try
        {
            memcpy(pBufOut, &st, sizeof(st));
            if (pdwActualOut != NULL)
                *pdwActualOut = sizeof(st);
            rc = TRUE;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            rc = FALSE;
        }
        return rc;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL WINAPI DllMain(HANDLE hInstance, DWORD dwReason, LPVOID pReserved)
{
    UNREFERENCED_PARAMETER(pReserved);
    if (dwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls((HMODULE)hInstance);
    return TRUE;
}
