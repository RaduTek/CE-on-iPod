//
// ipodlcd.h - display driver for the iPod classic LCD (ipod6g BSP)
//
// GPE-based flat driver (structure from the CEonPi raspiBasic_ddi /
// Microsoft ddi_hflat sample). GDI draws into a 320x240 RGB565 shadow
// framebuffer in RAM; every blit/line to the primary surface grows a dirty
// rectangle, and a flush thread pushes it to the panel GRAM.
//
#ifndef __IPODLCD_H
#define __IPODLCD_H

#include "lcdhw.h"

#define GPE_ZONE_PDD        DEBUGZONE(14)

// Delay between the first drawing call and the flush, to batch drawing
#define LCD_FLUSH_DELAY_MS  15

class IpodLcdGPE : public GPE
{
public:
    IpodLcdGPE();
    virtual ~IpodLcdGPE();

    virtual int     InVBlank();
    virtual int     NumModes();
    virtual SCODE   SetPalette(const PALETTEENTRY *, WORD, WORD);
    virtual SCODE   AllocSurface(GPESurf **, int, int, EGPEFormat, int);
    virtual SCODE   Line(GPELineParms *, EGPEPhase);
    virtual SCODE   BltPrepare(GPEBltParms *);
    virtual SCODE   BltComplete(GPEBltParms *);
    virtual SCODE   GetModeInfo(GPEMode *, int);
    virtual SCODE   SetMode(int, HPALETTE *);
    virtual SCODE   SetPointerShape(GPESurf *, GPESurf *, int, int, int, int);
    virtual SCODE   MovePointer(int, int);
    virtual VOID    PowerHandler(BOOL bOff);

    // flush thread body
    DWORD   FlushThread();

private:
    GPEMode             m_gpeMode;
    BOOL                m_hwReady;

    // dirty rectangle of the primary surface, protected by m_csDirty
    CRITICAL_SECTION    m_csDirty;
    BOOL                m_dirty;
    RECTL               m_dirtyRect;

    HANDLE              m_hFlushEvent;
    HANDLE              m_hFlushThread;

    // diagnostics (bring-up): drawing calls that reach the primary surface
    volatile LONG       m_bltCount;
    volatile LONG       m_lineCount;
    LONG                m_flushCount;
    LONG                m_flushFails;

    VOID    MarkDirty(const RECTL *pRect);
    VOID    MarkAllDirty();
    VOID    RequestFlush();
    VOID    FlushNow();
    VOID    DrawTestPattern();

    SCODE   TrackedEmulatedLine(GPELineParms *);
};

#endif // __IPODLCD_H
