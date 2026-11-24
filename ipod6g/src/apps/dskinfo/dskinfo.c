//
// dskinfo.c - the panel of the iPod's disk driver (ipoddsk.dll, "DSK1:")
//
// Shows, top to bottom, how far the disk gets into Windows: what the driver
// found and was asked, what the storage manager lists (store, partitions,
// file system), and what is in the mounted folder. "Verify file" reads
// \iPod\wince.ipod (the Windows CE image, about 21 MB) to its
// end and checks it against the checksum in its own header: a read through
// partition table, FAT and data area that proves itself. Nothing here
// writes.
//
#include <windows.h>
#include <winioctl.h>
#include <storemgr.h>

#include <ipoddsk.h>

#define IDC_REFRESH     101
#define IDC_VERIFY      102
#define LINE_H          12
#define BTN_H           22
#define CHUNK           65536
#define MODEL_NUMBER    71              // "ip6g" in the image header's checksum

static HWND     g_hWnd, g_hRefresh;
static HFONT    g_hFont;
static HANDLE   g_hDev = INVALID_HANDLE_VALUE;
static DWORD    g_devErr = 0;
static IPODDSK_STATUS g_st;
static BOOL     g_haveSt = FALSE;

static WCHAR    g_folder[MAX_PATH];     // the mounted folder, e.g. \iPod
static WCHAR    g_verify[160];          // result of the last "Verify file"
static BYTE     g_chunk[CHUNK];

static void Fetch(void)
{
    DWORD got = 0;

    if (g_hDev == INVALID_HANDLE_VALUE)
    {
        g_hDev = CreateFile(L"DSK1:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (g_hDev == INVALID_HANDLE_VALUE)
        {
            g_devErr = GetLastError();
            g_haveSt = FALSE;
            return;
        }
    }
    if (DeviceIoControl(g_hDev, IOCTL_IPODDSK_GET_STATUS, NULL, 0, &g_st, sizeof(g_st), &got, NULL)
        && got == sizeof(g_st))
        g_haveSt = TRUE;
    else
    {
        g_devErr = GetLastError();
        g_haveSt = FALSE;
    }
}

// The folder Windows mounted a volume under: a directory in the root that
// carries the "temporary" attribute. One called iPod is preferred.
static void FindFolder(void)
{
    WIN32_FIND_DATA fd;
    HANDLE h;

    g_folder[0] = 0;
    h = FindFirstFile(L"\\*", &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (fd.dwFileAttributes & FILE_ATTRIBUTE_TEMPORARY)
            && wcslen(fd.cFileName) < 40)
        {
            if (g_folder[0] == 0 || _wcsnicmp(fd.cFileName, L"iPod", 4) == 0)
                wsprintf(g_folder, L"\\%s", fd.cFileName);
        }
    } while (FindNextFile(h, &fd));
    FindClose(h);
}

// Reads the whole image file and compares with the checksum in its header.
static void VerifyFile(void)
{
    WCHAR path[MAX_PATH];
    HANDLE h;
    DWORD got, total = 0, sum = MODEL_NUMBER, want = 0, t0, ms, i;
    BOOL first = TRUE, ok = TRUE;

    FindFolder();
    if (g_folder[0] == 0)
    {
        wcscpy(g_verify, L"verify: no mounted folder");
        return;
    }
    wsprintf(path, L"%s\\wince.ipod", g_folder);
    h = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
    {
        wsprintf(g_verify, L"verify: cannot open %s (error %u)", path, GetLastError());
        return;
    }
    t0 = GetTickCount();
    for (;;)
    {
        if (!ReadFile(h, g_chunk, CHUNK, &got, NULL))
        {
            ok = FALSE;
            break;
        }
        if (got == 0)
            break;
        i = 0;
        if (first)
        {
            if (got < 8)
            {
                ok = FALSE;
                break;
            }
            want = ((DWORD)g_chunk[0] << 24) | ((DWORD)g_chunk[1] << 16) | ((DWORD)g_chunk[2] << 8) | g_chunk[3];
            i = 8;
            first = FALSE;
        }
        for (; i < got; i++)
            sum += g_chunk[i];
        total += got;
    }
    ms = GetTickCount() - t0;
    if (!ok)
        wsprintf(g_verify, L"verify: READ FAILED after %u bytes (error %u)", total, GetLastError());
    else
        wsprintf(g_verify, L"wince.ipod: %u bytes, %u.%u s, %u KB/s, checksum %s",
                 total, ms / 1000, (ms % 1000) / 100, ms ? total / ms : 0,
                 (sum == want && total > 8) ? L"OK" : L"WRONG");
    CloseHandle(h);
}

static void Line(HDC hdc, int *py, const WCHAR *text)
{
    RECT rc;

    SetRect(&rc, 3, *py, 317, *py + LINE_H);
    DrawText(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
    *py += LINE_H;
}

// What the storage manager lists: every store, and its partitions.
static void PaintStores(HDC hdc, int *py)
{
    STOREINFO si;
    PARTINFO pi;
    HANDLE hFind, hStore, hPart;
    WCHAR sz[200];
    int stores = 0, parts;

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    hFind = FindFirstStore(&si);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        wsprintf(sz, L"Windows lists NO store (error %u)", GetLastError());
        Line(hdc, py, sz);
        return;
    }
    do
    {
        if (++stores > 2)
            break;
        wsprintf(sz, L"store %s: %u x %u B, partitions %u, mounted %u, attr %X",
                 si.szDeviceName, (DWORD)si.snNumSectors, si.dwBytesPerSector, si.dwPartitionCount,
                 si.dwMountCount, si.dwAttributes);
        Line(hdc, py, sz);

        hStore = OpenStore(si.szDeviceName);
        if (hStore == INVALID_HANDLE_VALUE)
            continue;
        memset(&pi, 0, sizeof(pi));
        pi.cbSize = sizeof(pi);
        hPart = FindFirstPartition(hStore, &pi);
        parts = 0;
        if (hPart != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (++parts > 2)
                    break;
                pi.szPartitionName[10] = 0;
                pi.szFileSys[10] = 0;
                pi.szVolumeName[12] = 0;
                wsprintf(sz, L"  %s: type %02X, %s, \"%s\", attr %X %s", pi.szPartitionName, pi.bPartType,
                         pi.szFileSys[0] ? pi.szFileSys : L"no file system", pi.szVolumeName, pi.dwAttributes,
                         (pi.dwAttributes & PARTITION_ATTRIBUTE_MOUNTED) ? L"MOUNTED" : L"not mounted");
                Line(hdc, py, sz);
                memset(&pi, 0, sizeof(pi));
                pi.cbSize = sizeof(pi);
            } while (FindNextPartition(hPart, &pi));
            FindClosePartition(hPart);
        }
        CloseHandle(hStore);
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
    } while (FindNextStore(hFind, &si));
    FindCloseStore(hFind);
}

// The first names in the mounted folder.
static void PaintFolder(HDC hdc, int *py)
{
    WIN32_FIND_DATA fd;
    WCHAR sz[200], pattern[MAX_PATH];
    HANDLE h;
    int n = 0, len;

    FindFolder();
    if (g_folder[0] == 0)
    {
        Line(hdc, py, L"no mounted folder in \\");
        return;
    }
    wsprintf(pattern, L"%s\\*", g_folder);
    wsprintf(sz, L"%s:", g_folder);
    h = FindFirstFile(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
    {
        wsprintf(sz, L"%s: nothing listed (error %u)", g_folder, GetLastError());
        Line(hdc, py, sz);
        return;
    }
    do
    {
        n++;
        len = wcslen(sz);
        if (len < 58)
        {
            fd.cFileName[14] = 0;
            wsprintf(sz + len, L" %s%s", fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L"\\" : L"");
        }
    } while (FindNextFile(h, &fd) && n < 500);
    FindClose(h);
    Line(hdc, py, sz);
    wsprintf(sz, L"  %u%s entries in all", n, n >= 500 ? L"+" : L"");
    Line(hdc, py, sz);
}

static void Paint(HDC hdc)
{
    WCHAR sz[200];
    RECT rc;
    HFONT hOld;
    int y = BTN_H + 5;

    GetClientRect(g_hWnd, &rc);
    rc.top = BTN_H + 4;
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    hOld = (HFONT)SelectObject(hdc, g_hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    // the driver
    if (!g_haveSt)
    {
        wsprintf(sz, L"no answer from the driver DSK1: (error %u)", g_devErr);
        Line(hdc, &y, sz);
    }
    else
    {
        wsprintf(sz, L"driver %s (step %u), FAT32 %s, x%u: %u sectors of %u",
                 g_st.ready ? L"ready" : L"NOT READY", g_st.setupStep, g_st.fatFound ? L"found" : L"NOT found",
                 g_st.factor, g_st.totalSectors, g_st.bytesPerSector);
        Line(hdc, &y, sz);
        wsprintf(sz, L"reads %u, sectors %u, %u ms (longest %u), errors %u, bad %u",
                 g_st.reads, g_st.sectorsRead, g_st.readMs, g_st.maxReadMs, g_st.readErrors, g_st.badRequests);
        Line(hdc, &y, sz);
        wsprintf(sz, L"last read %u +%u; REFUSED: writes %u, formats %u",
                 g_st.lastStart, g_st.lastCount, g_st.writesRefused, g_st.formatsRefused);
        Line(hdc, &y, sz);
        wsprintf(sz, L"asked: info %u dev %u name %u set %u init %u flush %u other %u (%X)",
                 g_st.getInfos, g_st.deviceInfos, g_st.getNames, g_st.setInfos, g_st.initializeds,
                 g_st.flushes, g_st.others, g_st.lastOther);
        Line(hdc, &y, sz);
        if (g_st.ata.errors || !g_st.ready)
        {
            wsprintf(sz, L"disk: step %u code %u status %02X error %02X, errors %u",
                     g_st.ata.lastStep, g_st.ata.lastCode, g_st.ata.lastStatus, g_st.ata.lastError, g_st.ata.errors);
            Line(hdc, &y, sz);
        }
    }

    // Windows' view
    PaintStores(hdc, &y);
    PaintFolder(hdc, &y);
    if (g_verify[0])
        Line(hdc, &y, g_verify);

    SelectObject(hdc, hOld);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HINSTANCE hInst;
    HCURSOR hOld;
    HDC hdc;

    switch (msg)
    {
    case WM_CREATE:
        hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        g_hRefresh = CreateWindow(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                  4, 2, 80, BTN_H, hWnd, (HMENU)IDC_REFRESH, hInst, NULL);
        CreateWindow(L"BUTTON", L"Verify file (15 s)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                     90, 2, 130, BTN_H, hWnd, (HMENU)IDC_VERIFY, hInst, NULL);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_VERIFY)
        {
            wcscpy(g_verify, L"reading \\iPod\\wince.ipod to its end ...");
            InvalidateRect(hWnd, NULL, FALSE);
            UpdateWindow(hWnd);
            hOld = SetCursor(LoadCursor(NULL, IDC_WAIT));
            VerifyFile();
            SetCursor(hOld);
        }
        Fetch();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_SETFOCUS:
        if (g_hRefresh != NULL)
            SetFocus(g_hRefresh);
        return 0;

    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        Paint(hdc);
        EndPaint(hWnd, &ps);
        return 0;

    case WM_DESTROY:
        if (g_hDev != INVALID_HANDLE_VALUE)
        {
            CloseHandle(g_hDev);
            g_hDev = INVALID_HANDLE_VALUE;
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
    LOGFONT lf;
    RECT work;
    MSG msg;

    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pszCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    g_hWnd = FindWindow(L"IpodDskInfo", NULL);
    if (g_hWnd != NULL)
    {
        SetForegroundWindow(g_hWnd);
        return 0;
    }

    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -10;
    lf.lfWeight = FW_NORMAL;
    wcscpy(lf.lfFaceName, L"Tahoma");
    g_hFont = CreateFontIndirect(&lf);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"IpodDskInfo";
    if (!RegisterClass(&wc))
        return 1;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - 26);
    g_hWnd = CreateWindow(wc.lpszClassName, L"iPod disk",
                          WS_VISIBLE | WS_CAPTION | WS_SYSMENU,
                          work.left, work.top, work.right - work.left, work.bottom - work.top,
                          NULL, NULL, hInstance, NULL);
    if (g_hWnd == NULL)
        return 1;
    Fetch();
    InvalidateRect(g_hWnd, NULL, FALSE);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
