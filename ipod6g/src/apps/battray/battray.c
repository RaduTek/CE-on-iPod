//
// battray.c - a battery level icon in the taskbar, always there
//
// Windows CE's own taskbar shows a power icon only on external power or
// when the battery is low; on a healthy battery it shows nothing at all.
// This small program adds an icon that is always there: a battery outline
// filled to the level Windows reports (blue while on external power),
// redrawn every 15 seconds. Its tip says the percentage; tapping it opens
// the power status panel. It asks Windows, not the driver.
//
#include <windows.h>
#include <shellapi.h>

#define WM_TRAY         (WM_USER + 1)
#define ICON_SIZE       16
#define REFRESH_MS      15000
#define RETRY_MS        2000            // while the taskbar is not there yet

static HWND  g_hWnd;
static HICON g_hIcon = NULL;
static BOOL  g_added = FALSE;
static int   g_shownPercent = -2;
static BOOL  g_shownExternal = FALSE;

static void Fill(HDC hdc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    HBRUSH hbr = CreateSolidBrush(c);

    SetRect(&rc, l, t, r, b);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
}

// A 16 x 16 battery: body 14 x 8 with a 2-pixel button on the right, filled
// from the left. percent < 0 = unknown (grey inside).
static HICON MakeIcon(int percent, BOOL external)
{
    HDC hdcScreen = GetDC(NULL);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmColor = CreateCompatibleBitmap(hdcScreen, ICON_SIZE, ICON_SIZE);
    HBITMAP hbmMask = CreateBitmap(ICON_SIZE, ICON_SIZE, 1, 1, NULL);
    HBITMAP hOld;
    ICONINFO ii;
    HICON hIcon;
    COLORREF level;
    int width;

    // the mask: white = see-through everywhere but the battery
    hOld = (HBITMAP)SelectObject(hdc, hbmMask);
    PatBlt(hdc, 0, 0, ICON_SIZE, ICON_SIZE, WHITENESS);
    PatBlt(hdc, 0, 4, 14, 8, BLACKNESS);
    PatBlt(hdc, 14, 6, 2, 4, BLACKNESS);

    // the picture: black where it is see-through, then outline, inside, level
    SelectObject(hdc, hbmColor);
    PatBlt(hdc, 0, 0, ICON_SIZE, ICON_SIZE, BLACKNESS);
    Fill(hdc, 0, 4, 14, 12, RGB(0, 0, 0));
    Fill(hdc, 14, 6, 16, 10, RGB(0, 0, 0));
    Fill(hdc, 1, 5, 13, 11, (percent < 0) ? RGB(160, 160, 160) : RGB(255, 255, 255));
    if (percent >= 0)
    {
        if (external)
            level = RGB(0, 90, 210);
        else if (percent > 20)
            level = RGB(0, 150, 0);
        else if (percent > 5)
            level = RGB(230, 140, 0);
        else
            level = RGB(210, 0, 0);
        width = (percent * 12 + 50) / 100;
        if (width < 1)
            width = 1;
        Fill(hdc, 1, 5, 1 + width, 11, level);
    }
    SelectObject(hdc, hOld);

    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    DeleteDC(hdc);
    ReleaseDC(NULL, hdcScreen);
    return hIcon;
}

// Ask Windows, redraw if something changed, and put the icon in the taskbar.
static void Update(void)
{
    SYSTEM_POWER_STATUS_EX2 sps;
    NOTIFYICONDATA nid;
    HICON hNew;
    int percent = -1;
    BOOL external = FALSE;

    memset(&sps, 0, sizeof(sps));
    if (GetSystemPowerStatusEx2(&sps, sizeof(sps), FALSE) != 0)
    {
        if (sps.BatteryLifePercent <= 100)
            percent = sps.BatteryLifePercent;
        external = (sps.ACLineStatus == AC_LINE_ONLINE);
    }
    if (g_added && percent == g_shownPercent && external == g_shownExternal)
        return;

    hNew = MakeIcon(percent, external);
    if (hNew == NULL)
        return;

    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hWnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = hNew;
    if (percent < 0)
        wcscpy(nid.szTip, L"Battery: unknown");
    else
        wsprintf(nid.szTip, L"Battery %u %%%s, %u.%02u V", percent, external ? L" (external power)" : L"",
                 sps.BatteryVoltage / 1000, (sps.BatteryVoltage % 1000) / 10);

    if (Shell_NotifyIcon(g_added ? NIM_MODIFY : NIM_ADD, &nid))
    {
        if (!g_added)
        {
            g_added = TRUE;
            KillTimer(g_hWnd, 1);
            SetTimer(g_hWnd, 1, REFRESH_MS, NULL);
        }
        g_shownPercent = percent;
        g_shownExternal = external;
        if (g_hIcon != NULL)
            DestroyIcon(g_hIcon);
        g_hIcon = hNew;
    }
    else
    {
        DestroyIcon(hNew);          // no taskbar yet: the timer tries again
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PROCESS_INFORMATION pi;
    NOTIFYICONDATA nid;

    switch (msg)
    {
    case WM_TIMER:
        Update();
        return 0;

    case WM_TRAY:
        if (lParam == WM_LBUTTONUP)
        {
            if (CreateProcess(L"\\Windows\\pwrinfo.exe", NULL, NULL, NULL, FALSE, 0, NULL, NULL, NULL, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
        }
        return 0;

    case WM_DESTROY:
        if (g_added)
        {
            memset(&nid, 0, sizeof(nid));
            nid.cbSize = sizeof(nid);
            nid.hWnd = hWnd;
            nid.uID = 1;
            Shell_NotifyIcon(NIM_DELETE, &nid);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR pszCmdLine, int nCmdShow)
{
    WNDCLASS wc;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nCmdShow);

    // started from the registry's start-up list: say so, nothing waits on more
    if (pszCmdLine != NULL && pszCmdLine[0] >= L'0' && pszCmdLine[0] <= L'9')
        SignalStarted(_wtol(pszCmdLine));

    if (FindWindow(L"IpodBatTray", NULL) != NULL)
        return 0;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"IpodBatTray";
    if (!RegisterClass(&wc))
        return 1;
    g_hWnd = CreateWindow(wc.lpszClassName, L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (g_hWnd == NULL)
        return 1;

    SetTimer(g_hWnd, 1, RETRY_MS, NULL);
    Update();

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
