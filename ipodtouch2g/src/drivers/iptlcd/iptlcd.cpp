//
// iptlcd.cpp - display driver for the iPod touch 2G LCD (ipodtouch2g BSP)
//
// Structure from the ipod6g ipodlcd driver (itself from the CEonPi
// raspiBasic_ddi / Microsoft ddi_hflat sample). The cached framebuffer with
// a D-cache write-back after every drawing call follows CEonPi's vc4disp
// (verified there on the same ARM1176 core).
//
//  - The mode comes from the display controller's window 0 registers, as
//    iBoot left them (openiBoot clcd.c createWindow layout): format,
//    framebuffer address, line length, size. Verified values: RGB888,
//    0x0FC00000, 320, 320x480.
//  - The framebuffer must lie in the top 4 MB of DRAM that config.bib keeps
//    out of CE's RAM; it is then reached through the kernel's static cached
//    view (OEMAddressTable maps all DRAM). The driver is loaded in kernel
//    mode (platform.bib SHMK), so that address is valid here.
//  - No panel or backlight set-up: iBoot does that. No pointer.
//
#define DDI 1
#include <windows.h>
#include <types.h>
#include <winddi.h>
#include <gpe.h>
#include <emul.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <s5l8720.h>
#include "iptlcd.h"

//------------------------------------------------------------------------------
// Debug zones
//
INSTANTIATE_GPE_ZONES(0x0003, "iptlcd", "PDD", "Unused")

//------------------------------------------------------------------------------
// Statics
//
static GPE *g_pGPE = NULL;

// Channel masks of the current mode (red, green, blue)
static ULONG g_bitMasks[3] = { 0x00FF0000, 0x0000FF00, 0x000000FF };

BOOL APIENTRY GPEEnableDriver(
    ULONG version, ULONG cj, DRVENABLEDATA *pData, ENGCALLBACKS *pCallbacks);

//------------------------------------------------------------------------------
// DisplayInit: called by GWES once before anything else
//
BOOL DisplayInit(LPCTSTR pszInstance, DWORD monitors)
{
    RETAILMSG(1, (L"iptlcd: DisplayInit(%s, %d)\r\n",
                  pszInstance ? pszInstance : L"", monitors));
    g_pGPE = new IptLcdGPE;
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
// Status thread entry
//
static DWORD WINAPI StatusThreadProc(LPVOID pContext)
{
    return ((IptLcdGPE *)pContext)->StatusThread();
}

//------------------------------------------------------------------------------
// Constructor / destructor
//
IptLcdGPE::IptLcdGPE()
{
    memset(&m_gpeMode, 0, sizeof(m_gpeMode));
    m_fbPhys = 0;
    m_pFb = NULL;
    m_stride = 0;
    m_bytesPerPixel = 0;
    m_bltCount = 0;
    m_lineCount = 0;

    m_fHwOk = ReadController();
}

IptLcdGPE::~IptLcdGPE()
{
}

//------------------------------------------------------------------------------
// ReadController: take the mode from window 0 as iBoot set it up
//
BOOL IptLcdGPE::ReadController()
{
    PHYSICAL_ADDRESS pa;
    volatile UINT32 *pClcd;
    UINT32 format, addr, line, size;
    int width, height;

    pa.QuadPart = S5L_CLCD_PA;
    pClcd = (volatile UINT32 *)MmMapIoSpace(pa, 0x1000, FALSE);
    if (pClcd == NULL)
    {
        RETAILMSG(1, (L"iptlcd: cannot map the display controller\r\n"));
        return FALSE;
    }

    format = pClcd[CLCD_W0_FORMAT / 4];
    addr   = pClcd[CLCD_W0_ADDR / 4];
    line   = pClcd[CLCD_W0_STRIDE / 4];
    size   = pClcd[CLCD_W0_SIZE / 4];
    MmUnmapIoSpace((PVOID)pClcd, 0x1000);

    width  = (int)(size >> 16);
    height = (int)(size & 0xFFFF);

    RETAILMSG(1, (L"iptlcd: window 0: format 0x%08x, framebuffer 0x%08x, line %d, size %dx%d\r\n",
                  format, addr, line, width, height));

    switch ((format >> 8) & 0x0F)
    {
    case CLCD_FORMAT_RGB888:
        m_bytesPerPixel = 4;
        m_gpeMode.Bpp = 32;
        m_gpeMode.format = gpe32Bpp;
        g_bitMasks[0] = 0x00FF0000;
        g_bitMasks[1] = 0x0000FF00;
        g_bitMasks[2] = 0x000000FF;
        break;
    case CLCD_FORMAT_RGB565:
        m_bytesPerPixel = 2;
        m_gpeMode.Bpp = 16;
        m_gpeMode.format = gpe16Bpp;
        g_bitMasks[0] = 0xF800;
        g_bitMasks[1] = 0x07E0;
        g_bitMasks[2] = 0x001F;
        break;
    default:
        RETAILMSG(1, (L"iptlcd: unknown pixel format %d\r\n", (format >> 8) & 0x0F));
        return FALSE;
    }

    if (width <= 0 || width > 2048 || height <= 0 || height > 2048 ||
        (int)line < width || line > 2048)
    {
        RETAILMSG(1, (L"iptlcd: implausible window size\r\n"));
        return FALSE;
    }

    m_stride = (int)line * m_bytesPerPixel;

    // Only the reserved top of DRAM: anything else would be CE's RAM
    if (addr < S5L_FB_RESERVED_PA || (addr & 3) != 0 ||
        addr + (UINT32)(m_stride * height) > S5L_FB_RESERVED_PA + S5L_FB_RESERVED_SIZE)
    {
        RETAILMSG(1, (L"iptlcd: framebuffer 0x%08x is outside the reserved 0x%08x-0x%08x\r\n",
                      addr, S5L_FB_RESERVED_PA, S5L_FB_RESERVED_PA + S5L_FB_RESERVED_SIZE));
        return FALSE;
    }

    m_fbPhys = addr;
    m_pFb = (UINT8 *)S5L_DRAM_CA(addr);

    m_gpeMode.modeId    = 0;
    m_gpeMode.width     = width;
    m_gpeMode.height    = height;
    m_gpeMode.frequency = 60;
    return TRUE;
}

//------------------------------------------------------------------------------
// Modes: the one iBoot set up
//
int IptLcdGPE::NumModes()
{
    return m_fHwOk ? 1 : 0;
}

SCODE IptLcdGPE::GetModeInfo(GPEMode *pMode, int modeNumber)
{
    if (pMode == NULL || modeNumber != 0 || !m_fHwOk)
        return E_INVALIDARG;

    *pMode = m_gpeMode;
    return S_OK;
}

SCODE IptLcdGPE::SetMode(int modeNumber, HPALETTE *pPalette)
{
    RETAILMSG(1, (L"iptlcd: SetMode(%d)\r\n", modeNumber));

    if (!m_fHwOk || modeNumber != 0)
        return E_INVALIDARG;

    m_pMode = &m_gpeMode;
    m_nScreenWidth  = m_gpeMode.width;
    m_nScreenHeight = m_gpeMode.height;

    if (m_pPrimarySurface == NULL)
    {
        // nothing cached of the framebuffer yet; drop stray lines anyway
        CacheRangeFlush(NULL, 0, CACHE_SYNC_DISCARD);

        m_pPrimarySurface = new GPESurf(m_gpeMode.width, m_gpeMode.height,
                                        m_pFb, m_stride, m_gpeMode.format);
        if (m_pPrimarySurface == NULL)
        {
            RETAILMSG(1, (L"iptlcd: cannot create the primary surface\r\n"));
            return E_OUTOFMEMORY;
        }
        RETAILMSG(1, (L"iptlcd: %dx%d %d bpp, framebuffer 0x%08x (cached view 0x%08x), stride %d\r\n",
                      m_gpeMode.width, m_gpeMode.height, m_gpeMode.Bpp,
                      m_fbPhys, m_pFb, m_stride));
    }

    if (pPalette != NULL)
    {
        *pPalette = EngCreatePalette(PAL_BITFIELDS, 0, NULL,
                                     g_bitMasks[0], g_bitMasks[1], g_bitMasks[2]);
        if (*pPalette == NULL)
        {
            RETAILMSG(1, (L"iptlcd: EngCreatePalette failed\r\n"));
            return E_OUTOFMEMORY;
        }
    }

    DrawTestPattern();

    // bring-up: one status line 5 s later
    HANDLE hThread = CreateThread(NULL, 0, StatusThreadProc, this, 0, NULL);
    if (hThread != NULL)
        CloseHandle(hThread);

    return S_OK;
}

DWORD IptLcdGPE::StatusThread()
{
    Sleep(5000);
    RETAILMSG(1, (L"iptlcd: status 5 s after SetMode: %d blts and %d lines to the screen\r\n",
                  m_bltCount, m_lineCount));
    return 0;
}

//------------------------------------------------------------------------------
// Surfaces: everything else lives in system memory
//
SCODE IptLcdGPE::AllocSurface(
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
// FlushRect: write the cached framebuffer rows covering pRect back to
// memory (exclusive right/bottom, any order). NULL = the whole cache. The
// OAL cleans the whole 16 KB D-cache when the range is larger than the
// cache, so big rectangles cost no more than one full clean (as vc4disp).
//
VOID IptLcdGPE::FlushRect(const RECTL *pRect)
{
    LONG left, top, right, bottom;
    UINT8 *pStart, *pEnd;

    if (m_pPrimarySurface == NULL)
        return;

    if (pRect == NULL)
    {
        CacheRangeFlush(NULL, 0, CACHE_SYNC_WRITEBACK);
        return;
    }

    left   = min(pRect->left, pRect->right);
    right  = max(pRect->left, pRect->right);
    top    = min(pRect->top, pRect->bottom);
    bottom = max(pRect->top, pRect->bottom);

    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > m_gpeMode.width) right = m_gpeMode.width;
    if (bottom > m_gpeMode.height) bottom = m_gpeMode.height;
    if (left >= right || top >= bottom)
        return;

    // one contiguous range from the first touched byte to the last one
    pStart = m_pFb + top * m_stride + left * m_bytesPerPixel;
    pEnd   = m_pFb + (bottom - 1) * m_stride + right * m_bytesPerPixel;
    CacheRangeFlush(pStart, (DWORD)(pEnd - pStart), CACHE_SYNC_WRITEBACK);
}

//------------------------------------------------------------------------------
// Bring-up test pattern: 8 vertical colour bars (white, yellow, cyan, green,
// magenta, red, blue, black) over a grey ramp in the bottom quarter, shown
// for LCD_TEST_PATTERN_MS. Without it the screen starts black.
//
static UINT32 PackColour(UINT32 rgb, int bytesPerPixel)
{
    UINT32 r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;

    if (bytesPerPixel == 4)
        return rgb & 0x00FFFFFF;
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

VOID IptLcdGPE::DrawTestPattern()
{
    int width = m_gpeMode.width, height = m_gpeMode.height;
    int x, y;

#ifdef LCD_TEST_PATTERN_MS
    static const UINT32 bars[8] = {
        0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000
    };

    for (y = 0; y < height; y++)
    {
        UINT8 *pLine = m_pFb + y * m_stride;
        for (x = 0; x < width; x++)
        {
            UINT32 rgb;
            if (y < (height * 3) / 4)
            {
                rgb = bars[(x * 8) / width];
            }
            else
            {
                UINT32 v = (UINT32)((x * 255) / (width - 1));
                rgb = (v << 16) | (v << 8) | v;
            }
            if (m_bytesPerPixel == 4)
                ((UINT32 *)pLine)[x] = PackColour(rgb, 4);
            else
                ((UINT16 *)pLine)[x] = (UINT16)PackColour(rgb, 2);
        }
    }
    FlushRect(NULL);
    RETAILMSG(1, (L"iptlcd: test pattern shown (%d ms)\r\n", LCD_TEST_PATTERN_MS));
    Sleep(LCD_TEST_PATTERN_MS);
#endif

    // start from a known state: black screen
    for (y = 0; y < height; y++)
        memset(m_pFb + y * m_stride, 0, width * m_bytesPerPixel);
    FlushRect(NULL);
}

//------------------------------------------------------------------------------
// Blits: software (GPE emulation); write the destination rows back after
//
SCODE IptLcdGPE::BltPrepare(GPEBltParms *pBltParms)
{
    pBltParms->pBlt = &GPE::EmulatedBlt;

    if (pBltParms->pDst == m_pPrimarySurface)
    {
        LONG n = InterlockedIncrement(&m_bltCount);

        if (n <= 4)
        {
            const RECTL *p = pBltParms->prclDst;
            RETAILMSG(1, (L"iptlcd: blt #%d to screen, rop4 0x%04x, dst (%d,%d)-(%d,%d)\r\n",
                          n, pBltParms->rop4,
                          p ? p->left : -1, p ? p->top : -1,
                          p ? p->right : -1, p ? p->bottom : -1));
        }
    }
    return S_OK;
}

SCODE IptLcdGPE::BltComplete(GPEBltParms *pBltParms)
{
    if (pBltParms->pDst == m_pPrimarySurface)
        FlushRect(pBltParms->prclDst);
    return S_OK;
}

//------------------------------------------------------------------------------
// Lines: software; write back the bounding box of each segment
//
SCODE IptLcdGPE::TrackedEmulatedLine(GPELineParms *pParms)
{
    RECTL bounds;
    SCODE sc;
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
        bounds.right = m_gpeMode.width;
        bounds.bottom = m_gpeMode.height;
        break;
    }

    InterlockedIncrement(&m_lineCount);

    sc = EmulatedLine(pParms);
    FlushRect(&bounds);
    return sc;
}

SCODE IptLcdGPE::Line(GPELineParms *pLineParms, EGPEPhase phase)
{
    if (phase == gpeSingle || phase == gpePrepare)
    {
        if (pLineParms->pDst == m_pPrimarySurface)
            pLineParms->pLine =
                (SCODE (GPE::*)(struct GPELineParms *))&IptLcdGPE::TrackedEmulatedLine;
        else
            pLineParms->pLine = &GPE::EmulatedLine;
    }
    return S_OK;
}

//------------------------------------------------------------------------------
// No pointer, no palette, always "in vblank"
//
SCODE IptLcdGPE::SetPointerShape(
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

SCODE IptLcdGPE::MovePointer(int x, int y)
{
    UNREFERENCED_PARAMETER(x);
    UNREFERENCED_PARAMETER(y);
    return S_OK;
}

SCODE IptLcdGPE::SetPalette(const PALETTEENTRY *pSource, WORD firstEntry, WORD numEntries)
{
    UNREFERENCED_PARAMETER(pSource);
    UNREFERENCED_PARAMETER(firstEntry);
    UNREFERENCED_PARAMETER(numEntries);
    return S_OK;
}

int IptLcdGPE::InVBlank()
{
    return 1;
}

//------------------------------------------------------------------------------
// Power: panel sleep and backlight come with the PMU driver
//
VOID IptLcdGPE::PowerHandler(BOOL bOff)
{
    RETAILMSG(1, (L"iptlcd: PowerHandler(%d) - not implemented yet\r\n", bOff));
}
