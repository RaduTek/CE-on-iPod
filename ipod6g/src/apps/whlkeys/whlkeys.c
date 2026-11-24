//
// whlkeys.c - the iPod's click wheel as keys for Windows CE
//
// Started with Windows (HKLM\init). Reads the wheel driver's events (device
// WHL1:, ipodwhl.dll) and passes each on as a key, so Windows and every
// program can be worked without anything plugged in:
//
//   wheel            Down (clockwise) and Up, 24 to a turn
//   centre           Enter (held as long as the button is)
//   MENU             Escape; held for 0.6 s: the Windows key (Start menu)
//   play/pause       the keyboard key "play/pause"
//   next, previous   the keyboard keys "next track", "previous track"
//
// A program in front can ask for the wheel to be the volume instead (as an
// iPod's "now playing" screen does):
// then a turn sends the keyboard keys "volume up" (clockwise) and "volume
// down". See WM_WHLKEYS_MODE in ipodwhl.h.
//
// It has a window that is not shown. Starting the program a second time (the
// "Wheel" icon on the desktop) shows it: what the driver and this program
// have counted, for a photograph when something is wrong.
//
#include <windows.h>
#include <winioctl.h>

#include <ipodwhl.h>

#define LINE_H          13
#define MENU_HOLD_MS    600
#define WM_WHLKEYS_SHOW (WM_APP + 1)

static HWND   g_hWnd;
static HFONT  g_hFont;
static volatile LONG g_volumeMode;
static HWND   g_modeWnd;                    // who asked for volume keys
static DWORD  g_keys, g_steps, g_presses, g_reads, g_openErr;
static BOOL   g_devOpen;
static DWORD  g_lastVk;

static void Key(BYTE vk, BOOL down)
{
    keybd_event(vk, 0, down ? 0 : KEYEVENTF_KEYUP, 0);
    if (down)
    {
        g_keys++;
        g_lastVk = vk;
    }
}

static void Tap(BYTE vk)
{
    Key(vk, TRUE);
    Key(vk, FALSE);
}

// Volume keys from the wheel? Only while the window that asked for them is
// there and in front: a program that closed, crashed or went to the back
// must not leave the wheel useless for getting around.
static BOOL VolumeNow(void)
{
    HWND h = g_modeWnd;

    return g_volumeMode && h != NULL && IsWindow(h) && GetForegroundWindow() == h;
}

static DWORD WINAPI ReaderThread(LPVOID pv)
{
    HANDLE hDev = INVALID_HANDLE_VALUE;
    WHL_EVENT ev[16];
    DWORD got, i, menuTick = 0;
    BOOL menuDown = FALSE, menuFired = FALSE, centreDown = FALSE;

    UNREFERENCED_PARAMETER(pv);

    while (hDev == INVALID_HANDLE_VALUE)
    {
        hDev = CreateFile(IPODWHL_DEVICE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDev == INVALID_HANDLE_VALUE)
        {
            g_openErr = GetLastError();
            Sleep(500);
        }
    }
    g_devOpen = TRUE;

    for (;;)
    {
        got = 0;
        if (!ReadFile(hDev, ev, sizeof(ev), &got, NULL))
        {
            Sleep(250);
            continue;
        }
        g_reads++;
        for (i = 0; i < got / sizeof(WHL_EVENT); i++)
        {
            if (ev[i].kind == WHL_EV_STEP)
            {
                g_steps++;
                if (VolumeNow())
                    Tap((BYTE)(ev[i].a ? VK_VOLUME_UP : VK_VOLUME_DOWN));
                else
                    Tap((BYTE)(ev[i].a ? VK_DOWN : VK_UP));
            }
            else if (ev[i].kind == WHL_EV_BUTTON)
            {
                if (ev[i].b)
                    g_presses++;
                switch (ev[i].a)
                {
                case WHL_BTN_CENTRE:
                    // let go without a press seen (held since before Windows started): nothing
                    if (ev[i].b || centreDown)
                        Key(VK_RETURN, ev[i].b != 0);
                    centreDown = (ev[i].b != 0);
                    break;
                case WHL_BTN_MENU:
                    if (ev[i].b)
                    {
                        menuDown = TRUE;
                        menuFired = FALSE;
                        menuTick = GetTickCount();
                    }
                    else
                    {
                        if (menuDown && !menuFired)
                            Tap(VK_ESCAPE);
                        menuDown = FALSE;
                    }
                    break;
                case WHL_BTN_PLAY:
                    if (ev[i].b)
                        Tap(VK_MEDIA_PLAY_PAUSE);
                    break;
                case WHL_BTN_NEXT:
                    if (ev[i].b)
                        Tap(VK_MEDIA_NEXT_TRACK);
                    break;
                case WHL_BTN_PREVIOUS:
                    if (ev[i].b)
                        Tap(VK_MEDIA_PREV_TRACK);
                    break;
                }
            }
        }
        // MENU held long enough: the Start menu, once
        if (menuDown && !menuFired && GetTickCount() - menuTick >= MENU_HOLD_MS)
        {
            Tap(VK_LWIN);
            menuFired = TRUE;
        }
    }
}

static void Line(HDC hdc, int *py, const WCHAR *text)
{
    RECT rc;

    SetRect(&rc, 3, *py, 317, *py + LINE_H);
    DrawText(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    *py += LINE_H;
}

static void Paint(HDC hdc)
{
    static const WCHAR *names[5] = { L" CENTRE", L" NEXT", L" PREVIOUS", L" PLAY", L" MENU" };
    IPODWHL_STATUS st;
    WCHAR sz[200], held[64];
    HANDLE h;
    RECT rc;
    HFONT hOld;
    DWORD got = 0;
    int y = 3, i;

    GetClientRect(g_hWnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    hOld = (HFONT)SelectObject(hdc, g_hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    h = CreateFile(IPODWHL_DEVICE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        wsprintf(sz, L"driver WHL1: NOT THERE (error %u)", GetLastError());
        Line(hdc, &y, sz);
    }
    else if (!DeviceIoControl(h, IOCTL_IPODWHL_GET_STATUS, NULL, 0, &st, sizeof(st), &got, NULL) || got != sizeof(st))
    {
        wsprintf(sz, L"driver WHL1: no account of itself (error %u)", GetLastError());
        Line(hdc, &y, sz);
    }
    else
    {
        wsprintf(sz, L"driver: registers %s, looking %s", st.mapped ? L"mapped" : L"NOT MAPPED", st.running ? L"yes" : L"NO");
        Line(hdc, &y, sz);
        wsprintf(sz, L"clock %s (%08X), pins %08X, no clock %u times",
                 (st.pwrcon1 & 2) ? L"OFF" : L"on", st.pwrcon1, st.pcon14, st.gateClosed);
        Line(hdc, &y, sz);
        wsprintf(sz, L"looks %u, flags seen %X, cleared %u, longest gap %u ms", st.looks, st.flagsSeen, st.cleared, st.maxGapMs);
        Line(hdc, &y, sz);
        wsprintf(sz, L"reports %u (last %08X), events %u, lost %u", st.reports, st.lastRx, st.events, st.dropped);
        Line(hdc, &y, sz);
        held[0] = 0;
        for (i = 4; i >= 0; i--)
        {
            if (st.buttons & (1u << i))
                wcscat(held, names[i]);
        }
        if (held[0] == 0)
            wcscpy(held, L" none");
        if (st.touched)
            wsprintf(sz, L"now: held%s, wheel at %u", held, st.pos);
        else
            wsprintf(sz, L"now: held%s, wheel not touched", held);
        Line(hdc, &y, sz);
    }
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    y += 4;
    if (g_devOpen)
        wsprintf(sz, L"keys: reads %u, steps %u, presses %u, keys sent %u (last %02X)", g_reads, g_steps, g_presses, g_keys, g_lastVk);
    else
        wsprintf(sz, L"keys: the driver could NOT be opened (error %u)", g_openErr);
    Line(hdc, &y, sz);
    Line(hdc, &y, VolumeNow() ? L"the wheel is the volume" :
                  g_volumeMode ? L"the wheel is the arrow keys (volume asked for, asker not in front)" :
                  L"the wheel is the arrow keys");
    SelectObject(hdc, hOld);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;

    switch (msg)
    {
    case WM_WHLKEYS_SHOW:
        ShowWindow(hWnd, SW_SHOW);
        SetForegroundWindow(hWnd);
        SetTimer(hWnd, 1, 300, NULL);
        return 0;

    case WM_WHLKEYS_MODE:
        g_modeWnd = (HWND)lParam;
        g_volumeMode = (wParam != 0 && lParam != 0);
        return 0;

    case WM_TIMER:
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        Paint(hdc);
        EndPaint(hWnd, &ps);
        return 0;

    case WM_CLOSE:
        // closing puts the window away; the program goes on turning the wheel into keys
        KillTimer(hWnd, 1);
        ShowWindow(hWnd, SW_HIDE);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR pszCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    LOGFONT lf;
    RECT work;
    HWND hOther;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    // started from HKLM\init: tell the kernel the next ones may start
    if (pszCmdLine != NULL && pszCmdLine[0] >= L'0' && pszCmdLine[0] <= L'9')
        SignalStarted(_wtol(pszCmdLine));

    // already running: this start is the wish to see its window
    hOther = FindWindow(WHLKEYS_CLASS, NULL);
    if (hOther != NULL)
    {
        PostMessage(hOther, WM_WHLKEYS_SHOW, 0, 0);
        return 0;
    }

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -11;
    lf.lfWeight = FW_NORMAL;
    wcscpy(lf.lfFaceName, L"Tahoma");
    g_hFont = CreateFontIndirect(&lf);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = WHLKEYS_CLASS;
    if (!RegisterClass(&wc))
        return 1;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - 26);
    g_hWnd = CreateWindow(wc.lpszClassName, L"Wheel", WS_CAPTION | WS_SYSMENU,
                          work.left, work.top, work.right - work.left, work.bottom - work.top,
                          NULL, NULL, hInstance, NULL);
    if (g_hWnd == NULL)
        return 1;

    CloseHandle(CreateThread(NULL, 0, ReaderThread, NULL, 0, NULL));

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
