//
// wavpanel.c - the panel of the Windows CE sound driver (ipodwav.dll)
//
// "Tone" plays three seconds of a quiet tone through Windows' own wave API
// (22050 Hz mono, so the driver's mixer has to convert it to the codec's
// rate and to stereo). "Chime" lets Windows play its start-up sound file.
// The readout, refreshed twice a second, is what a photo of the screen has
// to carry back: what Windows answered at each step, and what the driver's
// hardware layer counted.
//
#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>

#include <ipodpcm.h>

#define IDC_TONE        101
#define IDC_CHIME       102
#define IDC_STOP        103
#define LINE_H          13
#define BTN_H           22

#define TONE_RATE       22050
#define TONE_MS         3000
#define TONE_SAMPLES    (TONE_RATE * (TONE_MS / 1000))
#define TONE_FADE       2205            // 100 ms
#define TONE_LEVEL      4               // 1/4 of full scale (-12 dB)
#define NOT_YET         0xFFFF

static HWND     g_hWnd, g_hTone;
static HFONT    g_hFont;
static HANDLE   g_hDev = INVALID_HANDLE_VALUE;
static IPODPCM_STATUS g_st;
static BOOL     g_haveSt = FALSE;
static DWORD    g_devErr = 0;

static HWAVEOUT g_hwo = NULL;
static WAVEHDR  g_hdr;
static short   *g_pTone = NULL;
static UINT     g_rcOpen = NOT_YET, g_rcPrep = NOT_YET, g_rcWrite = NOT_YET, g_rcClose = NOT_YET;
static UINT     g_tones = 0, g_done = 0;
static UINT     g_chimes = 0, g_chimeOk = 0;
static DWORD    g_chimeErr = 0;

// one period of a sine, amplitude 32767; every second point at 22050 Hz is 441 Hz
static const short g_sine[100] = {
         0,   2057,   4107,   6140,   8149,  10126,  12062,  13952,  15786,  17557,
     19260,  20886,  22431,  23886,  25247,  26509,  27666,  28714,  29648,  30466,
     31163,  31738,  32187,  32509,  32702,  32767,  32702,  32509,  32187,  31738,
     31163,  30466,  29648,  28714,  27666,  26509,  25247,  23886,  22431,  20886,
     19260,  17557,  15786,  13952,  12062,  10126,   8149,   6140,   4107,   2057,
         0,  -2057,  -4107,  -6140,  -8149, -10126, -12062, -13952, -15786, -17557,
    -19260, -20886, -22431, -23886, -25247, -26509, -27666, -28714, -29648, -30466,
    -31163, -31738, -32187, -32509, -32702, -32767, -32702, -32509, -32187, -31738,
    -31163, -30466, -29648, -28714, -27666, -26509, -25247, -23886, -22431, -20886,
    -19260, -17557, -15786, -13952, -12062, -10126,  -8149,  -6140,  -4107,  -2057
};

static void Fetch(void)
{
    DWORD got = 0;

    if (g_hDev == INVALID_HANDLE_VALUE)
    {
        g_hDev = CreateFile(L"WAV1:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (g_hDev == INVALID_HANDLE_VALUE)
        {
            g_devErr = GetLastError();
            return;
        }
    }
    if (DeviceIoControl(g_hDev, IOCTL_IPODPCM_GET_STATUS, NULL, 0, &g_st, sizeof(g_st), &got, NULL)
        && got == sizeof(g_st))
        g_haveSt = TRUE;
    else
        g_devErr = GetLastError();
}

// The tone is over (played to the end, or stopped): give everything back.
static void ToneCleanup(void)
{
    if (g_hwo != NULL)
    {
        waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
        g_rcClose = waveOutClose(g_hwo);
        g_hwo = NULL;
    }
    if (g_pTone != NULL)
    {
        LocalFree(g_pTone);
        g_pTone = NULL;
    }
}

static void ToneStart(void)
{
    WAVEFORMATEX wfx;
    int i, v;

    if (g_hwo != NULL)
        return;                         // one at a time

    g_rcOpen = g_rcPrep = g_rcWrite = g_rcClose = NOT_YET;
    g_tones++;

    g_pTone = (short *)LocalAlloc(LMEM_FIXED, TONE_SAMPLES * sizeof(short));
    if (g_pTone == NULL)
        return;
    for (i = 0; i < TONE_SAMPLES; i++)
    {
        v = g_sine[(i * 2) % 100] / TONE_LEVEL;
        if (i < TONE_FADE)
            v = v * i / TONE_FADE;
        else if (i >= TONE_SAMPLES - TONE_FADE)
            v = v * (TONE_SAMPLES - 1 - i) / TONE_FADE;
        g_pTone[i] = (short)v;
    }

    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 1;
    wfx.nSamplesPerSec = TONE_RATE;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 2;
    wfx.nAvgBytesPerSec = TONE_RATE * 2;

    g_rcOpen = waveOutOpen(&g_hwo, WAVE_MAPPER, &wfx, (DWORD)g_hWnd, 0, CALLBACK_WINDOW);
    if (g_rcOpen != MMSYSERR_NOERROR)
    {
        g_hwo = NULL;
        ToneCleanup();
        return;
    }
    memset(&g_hdr, 0, sizeof(g_hdr));
    g_hdr.lpData = (LPSTR)g_pTone;
    g_hdr.dwBufferLength = TONE_SAMPLES * sizeof(short);
    g_rcPrep = waveOutPrepareHeader(g_hwo, &g_hdr, sizeof(g_hdr));
    if (g_rcPrep == MMSYSERR_NOERROR)
        g_rcWrite = waveOutWrite(g_hwo, &g_hdr, sizeof(g_hdr));
    if (g_rcPrep != MMSYSERR_NOERROR || g_rcWrite != MMSYSERR_NOERROR)
        ToneCleanup();
}

static void AllStop(void)
{
    if (g_hwo != NULL)
        waveOutReset(g_hwo);            // the buffer comes back through MM_WOM_DONE
    PlaySound(NULL, NULL, 0);
}

static void Code(WCHAR *out, UINT rc)
{
    if (rc == NOT_YET)
        wcscpy(out, L"-");
    else
        wsprintf(out, L"%u", rc);
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
    WCHAR sz[160], a[12], b[12], c[12], d[12];
    WAVEOUTCAPS caps;
    RECT rc;
    HFONT hOld;
    DWORD vol = 0;
    UINT nDev, rcCaps, rcVol;
    int y = BTN_H + 6;

    GetClientRect(g_hWnd, &rc);
    rc.top = BTN_H + 4;
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    hOld = (HFONT)SelectObject(hdc, g_hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));

    // what Windows itself says
    nDev = waveOutGetNumDevs();
    memset(&caps, 0, sizeof(caps));
    rcCaps = nDev ? waveOutGetDevCaps(0, &caps, sizeof(caps)) : NOT_YET;
    rcVol = nDev ? waveOutGetVolume((HWAVEOUT)0, &vol) : NOT_YET;
    caps.szPname[20] = 0;
    wsprintf(sz, L"Windows: %u device  \"%s\"  volume %08X", nDev, rcCaps == 0 ? caps.szPname : L"?", vol);
    Line(hdc, &y, sz);
    Code(a, g_rcOpen); Code(b, g_rcPrep); Code(c, g_rcWrite); Code(d, g_rcClose);
    wsprintf(sz, L"tone %u: open %s  prepare %s  write %s  done %u  close %s%s",
             g_tones, a, b, c, g_done, d, g_hwo != NULL ? L"  PLAYING" : L"");
    Line(hdc, &y, sz);
    wsprintf(sz, L"chime %u: accepted %u  last error %u", g_chimes, g_chimeOk, g_chimeErr);
    Line(hdc, &y, sz);

    // what the driver's hardware layer says
    if (!g_haveSt)
    {
        wsprintf(sz, L"no answer from the driver WAV1: (error %u)", g_devErr);
        Line(hdc, &y, sz);
        SelectObject(hdc, hOld);
        return;
    }
    wsprintf(sz, L"driver: %s (code %u)  %u Hz (divider %u)  page %u",
             g_st.ready ? L"ready" : L"NOT READY", g_st.initRc, g_st.rate, g_st.clkDiv, g_st.pageFrames);
    Line(hdc, &y, sz);
    wsprintf(sz, L"%s  starts %u  stops %u  start code %u",
             g_st.running ? L"PLAYING" : L"idle", g_st.starts, g_st.stops, g_st.lastStartRc);
    Line(hdc, &y, sz);
    wsprintf(sz, L"pages %u  late %u  max gap %u ms  stalls %u", g_st.fills, g_st.lateFills, g_st.maxGapMs, g_st.stalls);
    Line(hdc, &y, sz);
    wsprintf(sz, L"DMA errors %u  stuck %u  peak %u (run before: %u)",
             g_st.dmaErrors, g_st.stuckWaits, g_st.peak, g_st.lastPeak);
    Line(hdc, &y, sz);
    wsprintf(sz, L"position %08X  buffer %08X", g_st.position, g_st.bufPhys);
    Line(hdc, &y, sz);
    wsprintf(sz, L"I2S txcon %08X  com %X  status %08X", g_st.i2sTxCon, g_st.i2sTxCom, g_st.i2sStatus);
    Line(hdc, &y, sz);
    wsprintf(sz, L"channel config %08X  control %08X  dma %X", g_st.chConfig, g_st.chControl, g_st.dmacConfig);
    Line(hdc, &y, sz);
    wsprintf(sz, L"gates %08X %08X  clkcon3 %08X", g_st.pwrcon0, g_st.pwrcon1, g_st.clkcon3);
    Line(hdc, &y, sz);

    SelectObject(hdc, hOld);
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PAINTSTRUCT ps;
    HINSTANCE hInst;
    HDC hdc;

    switch (msg)
    {
    case WM_CREATE:
        hInst = ((LPCREATESTRUCT)lParam)->hInstance;
        g_hTone = CreateWindow(L"BUTTON", L"Tone (3 s)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                               4, 2, 100, BTN_H, hWnd, (HMENU)IDC_TONE, hInst, NULL);
        CreateWindow(L"BUTTON", L"Chime", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                     110, 2, 80, BTN_H, hWnd, (HMENU)IDC_CHIME, hInst, NULL);
        CreateWindow(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                     196, 2, 70, BTN_H, hWnd, (HMENU)IDC_STOP, hInst, NULL);
        SetTimer(hWnd, 1, 500, NULL);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_TONE:
            ToneStart();
            break;
        case IDC_CHIME:
            g_chimes++;
            if (PlaySound(L"\\Windows\\startup.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT))
                g_chimeOk++;
            else
                g_chimeErr = GetLastError();
            break;
        case IDC_STOP:
            AllStop();
            break;
        }
        Fetch();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case MM_WOM_DONE:
        g_done++;
        ToneCleanup();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_SETFOCUS:
        if (g_hTone != NULL)
            SetFocus(g_hTone);
        return 0;

    case WM_TIMER:
        Fetch();
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_PAINT:
        hdc = BeginPaint(hWnd, &ps);
        Paint(hdc);
        EndPaint(hWnd, &ps);
        return 0;

    case WM_DESTROY:
        // a panel that goes away must not leave anything playing
        if (g_hwo != NULL)
        {
            waveOutReset(g_hwo);
            ToneCleanup();
        }
        PlaySound(NULL, NULL, 0);
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

    g_hWnd = FindWindow(L"IpodWavPanel", NULL);
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
    wc.lpszClassName = L"IpodWavPanel";
    if (!RegisterClass(&wc))
        return 1;

    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0))
        SetRect(&work, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) - 26);
    g_hWnd = CreateWindow(wc.lpszClassName, L"Sound check",
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
