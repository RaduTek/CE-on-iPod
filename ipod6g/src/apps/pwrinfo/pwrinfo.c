//
// pwrinfo.c - what Windows itself says about the clock and the battery
//
// The panel of the battery and clock driver (battdrvr.dll with ipodbatt.c).
// It talks to no driver: it shows Windows' own time and Windows' own power
// status, which is what the taskbar and every program get. Refreshed every
// two seconds.
//
#include <windows.h>
#include <winioctl.h>

#include <ipodbatt.h>

#define LINE_H          13

static HWND  g_hWnd;
static HFONT g_hFont;

static void Line(HDC hdc, int *py, const WCHAR *text)
{
    RECT rc;

    SetRect(&rc, 3, *py, 317, *py + LINE_H);
    DrawText(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    *py += LINE_H;
}

static void Paint(HDC hdc)
{
    static const WCHAR *days[7] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday", L"Thursday", L"Friday", L"Saturday" };
    SYSTEM_POWER_STATUS_EX2 sps;
    SYSTEMTIME st;
    WCHAR sz[200];
    RECT rc;
    HFONT hOld;
    DWORD got;
    int y = 4;

    GetClientRect(g_hWnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    hOld = (HFONT)SelectObject(hdc, g_hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    GetLocalTime(&st);
    wsprintf(sz, L"Windows' clock: %s %u-%02u-%02u  %02u:%02u:%02u", days[st.wDayOfWeek % 7],
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    Line(hdc, &y, sz);
    wsprintf(sz, L"running for %u s", GetTickCount() / 1000);
    Line(hdc, &y, sz);
    y += 4;

    memset(&sps, 0, sizeof(sps));
    got = GetSystemPowerStatusEx2(&sps, sizeof(sps), TRUE);
    if (got == 0)
    {
        wsprintf(sz, L"Windows has NO power status (error %u)", GetLastError());
        Line(hdc, &y, sz);
    }
    else
    {
        wsprintf(sz, L"external power: %s",
                 sps.ACLineStatus == AC_LINE_ONLINE ? L"connected" :
                 sps.ACLineStatus == AC_LINE_OFFLINE ? L"none (on battery)" : L"unknown");
        Line(hdc, &y, sz);
        if (sps.BatteryLifePercent == BATTERY_PERCENTAGE_UNKNOWN)
            Line(hdc, &y, L"battery: level UNKNOWN (no measurement)");
        else
        {
            wsprintf(sz, L"battery: %u %%, %u mV", sps.BatteryLifePercent, sps.BatteryVoltage);
            Line(hdc, &y, sz);
        }
        wsprintf(sz, L"battery state:%s%s%s%s%s  (flag %02X)",
                 (sps.BatteryFlag != BATTERY_FLAG_UNKNOWN && (sps.BatteryFlag & BATTERY_FLAG_HIGH)) ? L" high" : L"",
                 (sps.BatteryFlag != BATTERY_FLAG_UNKNOWN && (sps.BatteryFlag & BATTERY_FLAG_LOW)) ? L" low" : L"",
                 (sps.BatteryFlag != BATTERY_FLAG_UNKNOWN && (sps.BatteryFlag & BATTERY_FLAG_CRITICAL)) ? L" CRITICAL" : L"",
                 (sps.BatteryFlag != BATTERY_FLAG_UNKNOWN && (sps.BatteryFlag & BATTERY_FLAG_CHARGING)) ? L" charging" : L"",
                 sps.BatteryFlag == BATTERY_FLAG_UNKNOWN ? L" unknown" : L"", sps.BatteryFlag);
        Line(hdc, &y, sz);
    }
    y += 4;

    // what the driver itself did
    {
        IPODBATT_STATUS bs;
        HANDLE h = CreateFile(L"BAT1:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

        got = 0;
        if (h == INVALID_HANDLE_VALUE)
        {
            wsprintf(sz, L"driver BAT1: NOT THERE (error %u)", GetLastError());
            Line(hdc, &y, sz);
        }
        else if (!DeviceIoControl(h, IOCTL_IPODBATT_GET_STATUS, NULL, 0, &bs, sizeof(bs), &got, NULL) || got != sizeof(bs))
        {
            wsprintf(sz, L"driver BAT1: no account of itself (error %u)", GetLastError());
            Line(hdc, &y, sz);
        }
        else
        {
            wsprintf(sz, L"driver: chip clock %s, %s, Windows %s (error %u)",
                     bs.clockRead ? L"read" : L"NOT READ", bs.clockValid ? L"a date" : L"NOT A DATE",
                     bs.timeAccepted ? L"took it" : L"DID NOT TAKE IT", bs.timeError);
            Line(hdc, &y, sz);
            wsprintf(sz, L"  20%02X-%02X-%02X %02X:%02X:%02X at start-up", bs.clockRaw[6], bs.clockRaw[5], bs.clockRaw[4],
                     bs.clockRaw[2], bs.clockRaw[1], bs.clockRaw[0]);
            Line(hdc, &y, sz);
            wsprintf(sz, L"driver: asked %u, measured %u, failed %u, charger %02X, pin %s",
                     bs.asked, bs.measures, bs.fails, bs.mbcs1, bs.charging ? L"charging" : L"not charging");
            Line(hdc, &y, sz);
            wsprintf(sz, L"bus I2C1: reads %u, writes %u, errors %u, refused %u, open fails %u",
                     bs.bus.reads, bs.bus.writes, bs.bus.errors, bs.bus.refused, bs.busOpenFails);
            Line(hdc, &y, sz);
            wsprintf(sz, L"  last failure: chip %02X reg %02X step %u code %u",
                     bs.bus.lastChip, bs.bus.lastReg, bs.bus.lastStep, bs.bus.lastCode);
            Line(hdc, &y, sz);
        }
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    SelectObject(hdc, hOld);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HDC hdc;

    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hWnd, 1, 2000, NULL);
        return 0;

    case WM_TIMER:
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        Paint(hdc);
        EndPaint(hWnd, &ps);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
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
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pszCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    g_hWnd = FindWindow(L"IpodPwrInfo", NULL);
    if (g_hWnd != NULL)
    {
        SetForegroundWindow(g_hWnd);
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
    wc.lpszClassName = L"IpodPwrInfo";
    if (!RegisterClass(&wc))
        return 1;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - 26);
    g_hWnd = CreateWindow(wc.lpszClassName, L"Power status",
                          WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
                          work.left, work.top, work.right - work.left, work.bottom - work.top,
                          NULL, NULL, hInstance, NULL);
    if (g_hWnd == NULL)
        return 1;

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
