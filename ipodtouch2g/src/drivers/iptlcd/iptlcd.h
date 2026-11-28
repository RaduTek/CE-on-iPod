//
// iptlcd.h - display driver for the iPod touch 2G LCD (ipodtouch2g BSP)
//
// GPE flat driver (structure from the ipod6g ipodlcd driver and the CEonPi
// vc4disp driver). The display controller scans a framebuffer in DRAM that
// iBoot set up (window 0, verified 2026-11-28: 320x480 RGB888 at PA
// 0x0FC00000). GDI draws into it directly through the cached kernel view
// and the driver writes the touched rows back to memory after every blit
// and line, so the controller shows current pixels.
//
#ifndef __IPTLCD_H
#define __IPTLCD_H

#define GPE_ZONE_PDD        DEBUGZONE(14)

class IptLcdGPE : public GPE
{
public:
    IptLcdGPE();
    virtual ~IptLcdGPE();

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

    // bring-up diagnostics thread body
    DWORD   StatusThread();

private:
    BOOL        m_fHwOk;            // framebuffer found and usable
    GPEMode     m_gpeMode;
    UINT32      m_fbPhys;           // framebuffer physical address
    UINT8      *m_pFb;              // cached kernel view of it
    int         m_stride;           // bytes per line
    int         m_bytesPerPixel;

    // diagnostics: drawing calls that reach the primary surface
    volatile LONG m_bltCount;
    volatile LONG m_lineCount;

    BOOL    ReadController();
    VOID    FlushRect(const RECTL *pRect);
    VOID    DrawTestPattern();

    SCODE   TrackedEmulatedLine(GPELineParms *);
};

#endif // __IPTLCD_H
