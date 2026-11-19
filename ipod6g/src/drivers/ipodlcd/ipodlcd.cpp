//
// ipodlcd.cpp - display driver for the iPod classic LCD (ipod6g BSP)
//
// Structure from the CEonPi raspiBasic_ddi driver (itself the Microsoft
// ddi_hflat sample). Differences:
//   - no hardware framebuffer: the primary surface is a RAM shadow buffer
//     and changed areas are pushed to the panel GRAM (lcdhw.cpp)
//   - mode information is fixed (320x240 RGB565), no OAL IOCTL_HAL_DDI
//   - no pointer (the iPod has no mouse)
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <emul.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include "ipodlcd.h"

//------------------------------------------------------------------------------
// Debug zones
//
INSTANTIATE_GPE_ZONES(0x0003, "ipodlcd", "PDD", "Unused")

//------------------------------------------------------------------------------
// Statics
//
static GPE *g_pGPE = NULL;

// RGB565 channel masks
static ULONG g_bitMasks[] = { 0xF800, 0x07E0, 0x001F };

BOOL APIENTRY GPEEnableDriver(
    ULONG version, ULONG cj, DRVENABLEDATA *pData, ENGCALLBACKS *pCallbacks);

//------------------------------------------------------------------------------
// DisplayInit: called by GWES once before anything else
//
BOOL DisplayInit(LPCTSTR pszInstance, DWORD monitors)
{
    RETAILMSG(1, (L"ipodlcd: DisplayInit(%s, %d)\r\n",
                  pszInstance ? pszInstance : L"", monitors));
    g_pGPE = new IpodLcdGPE;
    return (g_pGPE != NULL);
}

//------------------------------------------------------------------------------
// DrvEnableDriver: GPE does the work
//
BOOL DrvEnableDriver(
    ULONG version, ULONG cj, DRVENABLEDATA *pData, ENGCALLBACKS *pCallbacks)
{
    return GPEEnableDriver(version, cj, pData, pCallbacks);
}

ULONG *APIENTRY DrvGetMasks(DHPDEV dhpdev)
{
    UNREFERENCED_PARAMETER(dhpdev);
    return g_bitMasks;
}

VOID RegisterDDHALAPI()
{
}

GPE *GetGPE()
{
    return g_pGPE;
}

//------------------------------------------------------------------------------
// Flush thread entry
//
static DWORD WINAPI FlushThreadProc(LPVOID pContext)
{
    return ((IpodLcdGPE *)pContext)->FlushThread();
}

//------------------------------------------------------------------------------
// Constructor / destructor
//
IpodLcdGPE::IpodLcdGPE()
{
    memset(&m_gpeMode, 0, sizeof(m_gpeMode));
    m_dirty = FALSE;
    memset(&m_dirtyRect, 0, sizeof(m_dirtyRect));
    m_hFlushEvent = NULL;
    m_hFlushThread = NULL;
    m_bltCount = 0;
    m_lineCount = 0;
    m_flushCount = 0;
    m_flushFails = 0;

    InitializeCriticalSection(&m_csDirty);

    m_hwReady = LcdHwInit();
    if (!m_hwReady)
        RETAILMSG(1, (L"ipodlcd: LCD hardware init failed, drawing will not reach the panel\r\n"));
}

IpodLcdGPE::~IpodLcdGPE()
{
    DeleteCriticalSection(&m_csDirty);
}

//------------------------------------------------------------------------------
// Modes: one fixed mode, 320x240 RGB565
//
int IpodLcdGPE::NumModes()
{
    return 1;
}

SCODE IpodLcdGPE::GetModeInfo(GPEMode *pMode, int modeNumber)
{
    if (pMode == NULL || modeNumber != 0)
        return E_INVALIDARG;

    pMode->modeId    = 0;
    pMode->width     = LCD_WIDTH;
    pMode->height    = LCD_HEIGHT;
    pMode->Bpp       = 16;
    pMode->frequency = 60;
    pMode->format    = gpe16Bpp;
    return S_OK;
}

SCODE IpodLcdGPE::SetMode(int modeNumber, HPALETTE *pPalette)
{
    SCODE sc;

    RETAILMSG(1, (L"ipodlcd: SetMode(%d)\r\n", modeNumber));

    sc = GetModeInfo(&m_gpeMode, modeNumber);
    if (FAILED(sc))
        return sc;

    m_pMode = &m_gpeMode;
    m_nScreenWidth  = m_gpeMode.width;
    m_nScreenHeight = m_gpeMode.height;

    // primary surface = shadow framebuffer in RAM
    if (m_pPrimarySurface == NULL)
    {
        m_pPrimarySurface = new GPESurf(LCD_WIDTH, LCD_HEIGHT, gpe16Bpp);
        if (m_pPrimarySurface == NULL || m_pPrimarySurface->Buffer() == NULL)
        {
            RETAILMSG(1, (L"ipodlcd: cannot allocate the %dx%d shadow framebuffer\r\n",
                          LCD_WIDTH, LCD_HEIGHT));
            delete m_pPrimarySurface;
            m_pPrimarySurface = NULL;
            return E_OUTOFMEMORY;
        }
        RETAILMSG(1, (L"ipodlcd: framebuffer at 0x%08x, stride %d\r\n",
                      m_pPrimarySurface->Buffer(), m_pPrimarySurface->Stride()));
    }

    if (pPalette != NULL)
    {
        *pPalette = EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                                     g_bitMasks[0], g_bitMasks[1], g_bitMasks[2]);
        if (*pPalette == NULL)
        {
            RETAILMSG(1, (L"ipodlcd: EngCreatePalette failed\r\n"));
            return E_OUTOFMEMORY;
        }
    }

    // bring-up check that the panel path works before GWES paints
    DrawTestPattern();

    // flush thread
    if (m_hFlushEvent == NULL)
        m_hFlushEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hFlushThread == NULL && m_hFlushEvent != NULL)
    {
        m_hFlushThread = CreateThread(NULL, 0, FlushThreadProc, this, 0, NULL);
        if (m_hFlushThread == NULL)
            RETAILMSG(1, (L"ipodlcd: cannot create the flush thread\r\n"));
    }

    return S_OK;
}

//------------------------------------------------------------------------------
// Surfaces: everything lives in system memory
//
SCODE IpodLcdGPE::AllocSurface(
    GPESurf **ppSurf, int width, int height, EGPEFormat format, int flags)
{
    if ((flags & GPE_REQUIRE_VIDEO_MEMORY) != 0)
    {
        *ppSurf = NULL;
        return E_OUTOFMEMORY;
    }

    *ppSurf = new GPESurf(width, height, format);
    if (*ppSurf == NULL || (*ppSurf)->Buffer() == NULL)
    {
        delete *ppSurf;
        *ppSurf = NULL;
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

//------------------------------------------------------------------------------
// Dirty rectangle handling
//
VOID IpodLcdGPE::MarkDirty(const RECTL *pRect)
{
    RECTL r = *pRect;
    LONG t;

    // blit rectangles may be flipped
    if (r.left > r.right)  { t = r.left; r.left = r.right; r.right = t; }
    if (r.top > r.bottom)  { t = r.top;  r.top = r.bottom; r.bottom = t; }

    // clip to the screen
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > LCD_WIDTH) r.right = LCD_WIDTH;
    if (r.bottom > LCD_HEIGHT) r.bottom = LCD_HEIGHT;
    if (r.left >= r.right || r.top >= r.bottom)
        return;

    EnterCriticalSection(&m_csDirty);
    if (!m_dirty)
    {
        m_dirtyRect = r;
        m_dirty = TRUE;
    }
    else
    {
        if (r.left < m_dirtyRect.left) m_dirtyRect.left = r.left;
        if (r.top < m_dirtyRect.top) m_dirtyRect.top = r.top;
        if (r.right > m_dirtyRect.right) m_dirtyRect.right = r.right;
        if (r.bottom > m_dirtyRect.bottom) m_dirtyRect.bottom = r.bottom;
    }
    LeaveCriticalSection(&m_csDirty);
}

VOID IpodLcdGPE::MarkAllDirty()
{
    RECTL r = { 0, 0, LCD_WIDTH, LCD_HEIGHT };
    MarkDirty(&r);
}

VOID IpodLcdGPE::RequestFlush()
{
    if (m_hFlushEvent != NULL)
        SetEvent(m_hFlushEvent);
    else
        FlushNow();
}

VOID IpodLcdGPE::FlushNow()
{
    RECTL r;
    BOOL dirty;

    if (!m_hwReady || m_pPrimarySurface == NULL)
        return;

    EnterCriticalSection(&m_csDirty);
    dirty = m_dirty;
    r = m_dirtyRect;
    m_dirty = FALSE;
    LeaveCriticalSection(&m_csDirty);

    if (!dirty)
        return;

    DWORD t0 = GetTickCount();
    BOOL ok = LcdHwPushRect((const BYTE *)m_pPrimarySurface->Buffer(), m_pPrimarySurface->Stride(),
                            r.left, r.top, r.right - r.left, r.bottom - r.top);
    m_flushCount++;
    if (!ok)
        m_flushFails++;

    // bring-up diagnostics: the first flushes in detail, then every 256th
    if (m_flushCount <= 8 || (m_flushCount & 0xFF) == 0 || !ok)
    {
        RETAILMSG(1, (L"ipodlcd: flush #%d (%d,%d)-(%d,%d) %s in %d ms (blts %d, lines %d)\r\n",
                      m_flushCount, r.left, r.top, r.right, r.bottom,
                      ok ? L"ok" : L"FAILED", GetTickCount() - t0,
                      m_bltCount, m_lineCount));
    }
}

DWORD IpodLcdGPE::FlushThread()
{
    BOOL reported = FALSE;

    for (;;)
    {
        // first wait is bounded so a status line appears even if nothing draws
        DWORD rc = WaitForSingleObject(m_hFlushEvent, reported ? INFINITE : 5000);

        if (rc == WAIT_TIMEOUT)
        {
            RETAILMSG(1, (L"ipodlcd: status 5 s after SetMode: %d blts and %d lines to the screen, %d flushes (%d failed)\r\n",
                          m_bltCount, m_lineCount, m_flushCount, m_flushFails));
            reported = TRUE;
            continue;
        }
        if (rc != WAIT_OBJECT_0)
            break;

        // let a burst of drawing calls accumulate into one transfer
        Sleep(LCD_FLUSH_DELAY_MS);
        FlushNow();
    }
    return 0;
}

//------------------------------------------------------------------------------
// Bring-up test pattern: 8 vertical colour bars (white, yellow, cyan, green,
// magenta, red, blue, black) over a grey ramp in the bottom quarter.
//
VOID IpodLcdGPE::DrawTestPattern()
{
#ifdef LCD_TEST_PATTERN_MS
    static const UINT16 bars[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
    };
    BYTE *pFrame = (BYTE *)m_pPrimarySurface->Buffer();
    int stride = m_pPrimarySurface->Stride();
    int x, y;

    for (y = 0; y < LCD_HEIGHT; y++)
    {
        UINT16 *pLine = (UINT16 *)(pFrame + y * stride);
        for (x = 0; x < LCD_WIDTH; x++)
        {
            if (y < (LCD_HEIGHT * 3) / 4)
            {
                pLine[x] = bars[(x * 8) / LCD_WIDTH];
            }
            else
            {
                // grey ramp: 5/6/5 bits from the x position
                UINT16 v = (UINT16)((x * 32) / LCD_WIDTH);
                pLine[x] = (UINT16)((v << 11) | ((v << 1) << 5) | v);
            }
        }
    }

    MarkAllDirty();
    FlushNow();
    RETAILMSG(1, (L"ipodlcd: test pattern shown (%d ms)\r\n", LCD_TEST_PATTERN_MS));
    Sleep(LCD_TEST_PATTERN_MS);
#else
    // start from a known state: black screen
    memset(m_pPrimarySurface->Buffer(), 0, m_pPrimarySurface->Stride() * LCD_HEIGHT);
    MarkAllDirty();
    FlushNow();
#endif
}

//------------------------------------------------------------------------------
// Blits: software (GPE emulation); record the destination area
//
SCODE IpodLcdGPE::BltPrepare(GPEBltParms *pBltParms)
{
    pBltParms->pBlt = &GPE::EmulatedBlt;

    if (pBltParms->pDst == m_pPrimarySurface)
    {
        LONG n = InterlockedIncrement(&m_bltCount);

        if (n <= 4)
        {
            const RECTL *p = pBltParms->prclDst;
            RETAILMSG(1, (L"ipodlcd: blt #%d to screen, rop4 0x%04x, dst (%d,%d)-(%d,%d)\r\n",
                          n, pBltParms->rop4,
                          p ? p->left : -1, p ? p->top : -1,
                          p ? p->right : -1, p ? p->bottom : -1));
        }

        if (pBltParms->prclDst != NULL)
            MarkDirty(pBltParms->prclDst);
        else
            MarkAllDirty();
    }
    return S_OK;
}

SCODE IpodLcdGPE::BltComplete(GPEBltParms *pBltParms)
{
    if (pBltParms->pDst == m_pPrimarySurface)
        RequestFlush();
    return S_OK;
}

//------------------------------------------------------------------------------
// Lines: software; record the bounding box of each segment on the primary
//
SCODE IpodLcdGPE::TrackedEmulatedLine(GPELineParms *pParms)
{
    RECTL bounds;
    int n;

    // Bounding box of the segment, from the ddi_hflat cursor-overlap code
    if (pParms->dN && pParms->dM)
        n = 2 + (pParms->cPels * pParms->dN) / pParms->dM;
    else
        n = 1;

    switch (pParms->iDir)
    {
    case 0:
        bounds.left = pParms->xStart;
        bounds.top = pParms->yStart;
        bounds.right = pParms->xStart + pParms->cPels + 1;
        bounds.bottom = bounds.top + n;
        break;
    case 1:
        bounds.left = pParms->xStart;
        bounds.top = pParms->yStart;
        bounds.bottom = pParms->yStart + pParms->cPels + 1;
        bounds.right = bounds.left + n;
        break;
    case 2:
        bounds.right = pParms->xStart + 1;
        bounds.top = pParms->yStart;
        bounds.bottom = pParms->yStart + pParms->cPels + 1;
        bounds.left = bounds.right - n;
        break;
    case 3:
        bounds.right = pParms->xStart + 1;
        bounds.top = pParms->yStart;
        bounds.left = pParms->xStart - pParms->cPels;
        bounds.bottom = bounds.top + n;
        break;
    case 4:
        bounds.right = pParms->xStart + 1;
        bounds.bottom = pParms->yStart + 1;
        bounds.left = pParms->xStart - pParms->cPels;
        bounds.top = bounds.bottom - n;
        break;
    case 5:
        bounds.right = pParms->xStart + 1;
        bounds.bottom = pParms->yStart + 1;
        bounds.top = pParms->yStart - pParms->cPels;
        bounds.left = bounds.right - n;
        break;
    case 6:
        bounds.left = pParms->xStart;
        bounds.bottom = pParms->yStart + 1;
        bounds.top = pParms->yStart - pParms->cPels;
        bounds.right = bounds.left + n;
        break;
    case 7:
        bounds.left = pParms->xStart;
        bounds.bottom = pParms->yStart + 1;
        bounds.right = pParms->xStart + pParms->cPels + 1;
        bounds.top = bounds.bottom - n;
        break;
    default:
        bounds.left = 0;
        bounds.top = 0;
        bounds.right = LCD_WIDTH;
        bounds.bottom = LCD_HEIGHT;
        break;
    }

    MarkDirty(&bounds);
    InterlockedIncrement(&m_lineCount);

    SCODE sc = EmulatedLine(pParms);
    RequestFlush();
    return sc;
}

SCODE IpodLcdGPE::Line(GPELineParms *pLineParms, EGPEPhase phase)
{
    if (phase == gpeSingle || phase == gpePrepare)
    {
        if (pLineParms->pDst == m_pPrimarySurface)
            pLineParms->pLine =
                (SCODE (GPE::*)(struct GPELineParms *))&IpodLcdGPE::TrackedEmulatedLine;
        else
            pLineParms->pLine = &GPE::EmulatedLine;
    }
    return S_OK;
}

//------------------------------------------------------------------------------
// No pointer, no palette, always "in vblank"
//
SCODE IpodLcdGPE::SetPointerShape(
    GPESurf *pMask, GPESurf *pColorSurf, int xHotspot, int yHotspot, int xSize, int ySize)
{
    UNREFERENCED_PARAMETER(pMask);
    UNREFERENCED_PARAMETER(pColorSurf);
    UNREFERENCED_PARAMETER(xHotspot);
    UNREFERENCED_PARAMETER(yHotspot);
    UNREFERENCED_PARAMETER(xSize);
    UNREFERENCED_PARAMETER(ySize);
    return S_OK;
}

SCODE IpodLcdGPE::MovePointer(int x, int y)
{
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
    return S_OK;
}

SCODE IpodLcdGPE::SetPalette(const PALETTEENTRY *pSource, WORD firstEntry, WORD numEntries)
{
    UNREFERENCED_PARAMETER(pSource);
    UNREFERENCED_PARAMETER(firstEntry);
    UNREFERENCED_PARAMETER(numEntries);
    return S_OK;
}

int IpodLcdGPE::InVBlank()
{
    return 1;
}

//------------------------------------------------------------------------------
// Power: panel sleep and backlight come with the PMU driver
//
VOID IpodLcdGPE::PowerHandler(BOOL bOff)
{
    RETAILMSG(1, (L"ipodlcd: PowerHandler(%d) - not implemented yet\r\n", bOff));
}
