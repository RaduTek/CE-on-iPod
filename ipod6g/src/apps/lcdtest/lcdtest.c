//
// lcdtest.c - GDI test screen for the ipod6g display driver
//
// Draws straight onto the screen DC (no window needed, so it works without
// a shell): a full-screen fill, four coloured boxes, a diagonal line and a
// line of text, then moves a small box across the screen for a few seconds.
// Each step goes GDI -> ipodlcd blit/line -> dirty rectangle -> flush
// thread -> panel, so the driver's "flush #n" log lines should follow.
//
// Start from Platform Builder Target Control:  s lcdtest
//
#include <windows.h>

static void Fill(HDC hdc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc;
    HBRUSH hbr = CreateSolidBrush(c);

    SetRect(&rc, l, t, r, b);
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR pszCmdLine, int nCmdShow)
{
    HDC hdc;
    int w, h, i, x;
    RECT rc;
    HPEN hpen, hpenOld;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pszCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    hdc = GetDC(NULL);
    if (hdc == NULL)
    {
        RETAILMSG(1, (L"lcdtest: GetDC(NULL) failed, error %d\r\n", GetLastError()));
        return 1;
    }

    w = GetDeviceCaps(hdc, HORZRES);
    h = GetDeviceCaps(hdc, VERTRES);
    RETAILMSG(1, (L"lcdtest: screen %dx%d, %d bpp\r\n", w, h, GetDeviceCaps(hdc, BITSPIXEL)));

    // background
    Fill(hdc, 0, 0, w, h, RGB(0, 0, 96));

    // four boxes: red, green, blue, white
    Fill(hdc,  20,  20, 100, 100, RGB(255, 0, 0));
    Fill(hdc, 120,  20, 200, 100, RGB(0, 255, 0));
    Fill(hdc, 220,  20, 300, 100, RGB(0, 0, 255));
    Fill(hdc,  20, 120, 100, 200, RGB(255, 255, 255));

    // yellow diagonal line
    hpen = CreatePen(PS_SOLID, 3, RGB(255, 255, 0));
    hpenOld = (HPEN)SelectObject(hdc, hpen);
    MoveToEx(hdc, 120, 120, NULL);
    LineTo(hdc, 300, 200);
    SelectObject(hdc, hpenOld);
    DeleteObject(hpen);

    // text
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetRect(&rc, 0, h - 30, w, h);
    DrawText(hdc, L"Windows CE on iPod", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RETAILMSG(1, (L"lcdtest: static screen drawn, now moving a box for ~5 s\r\n"));

    // moving box: many small updates
    for (i = 0; i < 50; i++)
    {
        x = 110 + (i * 4) % 190;
        Fill(hdc, 110, 205, 310, 225, RGB(0, 0, 96));
        Fill(hdc, x, 207, x + 16, 223, RGB(255, 128, 0));
        Sleep(100);
    }

    ReleaseDC(NULL, hdc);
    RETAILMSG(1, (L"lcdtest: done\r\n"));
    return 0;
}
