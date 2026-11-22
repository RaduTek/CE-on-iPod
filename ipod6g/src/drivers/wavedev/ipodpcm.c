//
// ipodpcm.c - the iPod's audio path under the Windows CE sound driver
//
// The path is the one Rockbox plays music through (first tried under
// Windows CE with a plain tone): DMA controller 0, channel 2 -> I2S 0
// -> CS42L55 codec, 24-bit samples in 32-bit words. What Rockbox set up before
// it started Windows CE is left alone: the codec (powered, unmuted, at Rockbox's
// volume, and the clock master) and the clocks. The sample rate is whatever
// Rockbox left the codec at; it is read back from I2SCLKDIV (12 MHz / rate).
//
// Two pages of sound, each one DMA list item, linked to each other: the DMA
// controller plays page 0, 1, 0, 1 ... by itself, without an interrupt. The
// driver's thread looks at the DMA position every few milliseconds and
// refills the page the controller has just left.
//
// All functions are called with the driver's lock held, except
// IpodPcm_Init (start-up) and IpodPcm_StatusIoctl (reads only).
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <ipodpcm.h>

// physical addresses (Rockbox firmware/export/s5l87xx.h, S5L8702)
#define PA_CLK              0x3C500000
#define PA_I2S              0x3CA00000
#define PA_DMAC0            0x38200000

#define CLK_CLKCON3         0x0C
#define CLK_PWRCON0         0x48        // bit 25 = DMA controller 0 clock gate
#define CLK_PWRCON1         0x4C        // bit 7 = I2S 0 clock gate
#define PWRCON0_DMAC0       (1 << 25)
#define PWRCON1_I2S0        (1 << 7)

#define I2S_CLKCON          0x00
#define I2S_TXCON           0x04
#define I2S_TXCOM           0x08
#define I2S_TXDB0           0x10
#define I2S_STATUS          0x3C
#define I2S_CLKDIV          0x40
#define I2STXCON_24IN32     0x0B100059  // 24 bits in 32 (stock Rockbox: 16, 0x0B100019)
#define I2STXCOM_STOP       0x0A
#define I2STXCOM_RUN        0x0E
#define I2S_MCLK_HZ         12000000

#define DMAC_INTTCCLR       0x08
#define DMAC_INTERRCLR      0x10
#define DMAC_RAWINTERR      0x18
#define DMAC_CONFIG         0x30
#define DMAC_CH             2           // the channel Rockbox plays through
#define DMAC_CHBASE         (0x100 + (DMAC_CH << 5))
#define CH_SRCADDR          (DMAC_CHBASE + 0x00)
#define CH_DESTADDR         (DMAC_CHBASE + 0x04)
#define CH_LINK             (DMAC_CHBASE + 0x08)
#define CH_CONTROL          (DMAC_CHBASE + 0x0C)
#define CH_CONFIG           (DMAC_CHBASE + 0x10)
#define CHCONFIG_E          (1 << 0)
#define CHCONFIG_A          (1 << 17)
#define DMAC_PERI_I2S0_TX   0x0A

// one list item: 32-bit words, the source moves, the destination is fixed,
// bursts of 8 in and 4 out, no interrupt
#define CH_CONTROL_VALUE(words) ((words) | (2 << 12) | (1 << 15) | (2 << 18) | (2 << 21) \
                                 | (1 << 26) | (7 << 28))
// memory to peripheral, the DMA controller counts; both interrupts masked
#define CH_CONFIG_VALUE     ((DMAC_PERI_I2S0_TX << 6) | (1 << 11))

// the DMA memory: page 0, page 1, the two list items
#define PAGE1_OFFSET        0x4000
#define LLI_OFFSET          0x8000
#define DMA_BLOCK_BYTES     0x9000
#define IDLE_WAIT_LOOPS     200000
#define STALL_MS            500

static volatile UINT32 *g_pClk = NULL, *g_pI2s = NULL, *g_pDmac = NULL;
#define RCLK(off)           g_pClk[(off) >> 2]
#define RI2S(off)           g_pI2s[(off) >> 2]
#define RDMA(off)           g_pDmac[(off) >> 2]

static BYTE    *g_pBlock = NULL;        // the DMA memory (not cached)
static ULONG    g_bufPhys = 0;
static IPODPCM_STATUS g_st;
static DWORD    g_lastPoll, g_lastMove, g_lastSrc;
static BOOL     g_stalled;

static void *MapReg(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return MmMapIoSpace(a, 0x1000, FALSE);
}

// Disable the channel and wait until it is idle (bounded).
static void ChannelOff(void)
{
    int i;

    RDMA(CH_CONFIG) &= ~CHCONFIG_E;
    for (i = 0; i < IDLE_WAIT_LOOPS; i++)
    {
        if ((RDMA(CH_CONFIG) & CHCONFIG_A) == 0)
            break;
    }
    if (i == IDLE_WAIT_LOOPS)
        g_st.stuckWaits++;
    RDMA(DMAC_INTTCCLR) = 1 << DMAC_CH;
    RDMA(DMAC_INTERRCLR) = 1 << DMAC_CH;
}

// The page the DMA controller reads from, by its source address. An address
// just past the end of a page still counts as that page: the controller is
// about to load the next list item.
static DWORD PlayingPage(DWORD src)
{
    return ((src - g_bufPhys) >= PAGE1_OFFSET) ? 1 : 0;
}

BOOL IpodPcm_Init(void)
{
    DWORD div;

    memset(&g_st, 0, sizeof(g_st));
    g_st.version = IPODPCM_VERSION;
    g_st.rate = IPODPCM_FALLBACK_RATE;
    g_st.pageFrames = IPODPCM_FALLBACK_FRAMES;

    g_pClk = (volatile UINT32 *)MapReg(PA_CLK);
    g_pI2s = (volatile UINT32 *)MapReg(PA_I2S);
    g_pDmac = (volatile UINT32 *)MapReg(PA_DMAC0);
    if (g_pClk == NULL || g_pI2s == NULL || g_pDmac == NULL)
    {
        g_st.initRc = 1;
        RETAILMSG(1, (L"ipodwav: cannot map the registers\r\n"));
        return FALSE;
    }

    // the I2S block must have its clock for its registers to be read
    if (RCLK(CLK_PWRCON1) & PWRCON1_I2S0)
        RCLK(CLK_PWRCON1) &= ~PWRCON1_I2S0;

    // the rate Rockbox left the codec at; anything outside 8 .. 120 kHz is not believed
    div = RI2S(I2S_CLKDIV);
    g_st.clkDiv = div;
    if (div < 100 || div > 1500)
    {
        g_st.initRc = 2;
        RETAILMSG(1, (L"ipodwav: I2SCLKDIV is %u, no sample rate follows from that\r\n", div));
        return FALSE;
    }

    // one block of real memory that is not cached, for the DMA controller
    g_pBlock = (BYTE *)AllocPhysMem(DMA_BLOCK_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &g_bufPhys);
    if (g_pBlock == NULL)
    {
        g_st.initRc = 3;
        RETAILMSG(1, (L"ipodwav: no memory for the DMA pages\r\n"));
        return FALSE;
    }
    memset(g_pBlock, 0, DMA_BLOCK_BYTES);
    g_st.bufPhys = g_bufPhys;

    g_st.rate = (I2S_MCLK_HZ + div / 2) / div;
    g_st.pageFrames = g_st.rate / 50;
    if (g_st.pageFrames > IPODPCM_MAX_PAGE_FRAMES)
        g_st.pageFrames = IPODPCM_MAX_PAGE_FRAMES;
    g_st.ready = 1;
    RETAILMSG(1, (L"ipodwav: ready, I2SCLKDIV %u = %u Hz, pages of %u frames at physical 0x%08x\r\n",
                  div, g_st.rate, g_st.pageFrames, g_bufPhys));
    return TRUE;
}

DWORD IpodPcm_Rate(void)
{
    return g_st.rate ? g_st.rate : IPODPCM_FALLBACK_RATE;
}

DWORD IpodPcm_PageFrames(void)
{
    return g_st.pageFrames ? g_st.pageFrames : IPODPCM_FALLBACK_FRAMES;
}

void IpodPcm_WritePage(DWORD page, const short *pStereo)
{
    INT32 *pOut;
    DWORD i, n, peak;
    int v, a;

    if (!g_st.ready || page > 1 || pStereo == NULL)
        return;

    pOut = (INT32 *)(g_pBlock + (page ? PAGE1_OFFSET : 0));
    n = g_st.pageFrames * 2;
    peak = g_st.peak;
    for (i = 0; i < n; i++)
    {
        v = pStereo[i];
        a = (v < 0) ? -v : v;
        if ((DWORD)a > peak)
            peak = (DWORD)a;
        pOut[i] = (INT32)v * (1 << IPODPCM_HEADROOM_SHIFT);
    }
    g_st.peak = peak;
    g_st.fills++;

    // too late: the controller is already playing what was in this page
    if (g_st.running && PlayingPage(RDMA(CH_SRCADDR)) == page)
        g_st.lateFills++;
}

BOOL IpodPcm_Start(void)
{
    UINT32 *pLli;
    DWORD words;

    if (!g_st.ready)
    {
        g_st.lastStartRc = 1;
        return FALSE;
    }
    if (g_st.running)
        return TRUE;

    // clocks for the DMA controller and the I2S block (0 = running). Written
    // only when a gate is closed: the battery driver switches another bit of
    // PWRCON1 on and off, and two drivers rewriting one register can undo
    // each other's change.
    if (RCLK(CLK_PWRCON0) & PWRCON0_DMAC0)
        RCLK(CLK_PWRCON0) &= ~PWRCON0_DMAC0;
    if (RCLK(CLK_PWRCON1) & PWRCON1_I2S0)
        RCLK(CLK_PWRCON1) &= ~PWRCON1_I2S0;

    ChannelOff();
    RDMA(DMAC_CONFIG) |= 1;             // controller on (Rockbox turns it off when idle)

    // the I2S transmitter, 24 bits in 32, started before the DMA channel
    RI2S(I2S_TXCON) = I2STXCON_24IN32;
    RI2S(I2S_CLKCON) = 1;
    RI2S(I2S_TXCOM) = I2STXCOM_STOP;
    RI2S(I2S_TXCOM) = I2STXCOM_RUN;

    // two list items, one page each, linking to each other
    words = g_st.pageFrames * 2;
    pLli = (UINT32 *)(g_pBlock + LLI_OFFSET);
    pLli[0] = g_bufPhys;
    pLli[1] = PA_I2S + I2S_TXDB0;
    pLli[2] = g_bufPhys + LLI_OFFSET + 16;
    pLli[3] = CH_CONTROL_VALUE(words);
    pLli[4] = g_bufPhys + PAGE1_OFFSET;
    pLli[5] = PA_I2S + I2S_TXDB0;
    pLli[6] = g_bufPhys + LLI_OFFSET;
    pLli[7] = CH_CONTROL_VALUE(words);

    // the channel starts with the first item
    RDMA(CH_SRCADDR) = pLli[0];
    RDMA(CH_DESTADDR) = pLli[1];
    RDMA(CH_LINK) = pLli[2];
    RDMA(CH_CONTROL) = pLli[3];
    RDMA(CH_CONFIG) = CH_CONFIG_VALUE;
    RDMA(CH_CONFIG) = CH_CONFIG_VALUE | CHCONFIG_E;

    g_lastPoll = g_lastMove = GetTickCount();
    g_lastSrc = g_bufPhys;
    g_stalled = FALSE;
    g_st.lastStartRc = 0;
    g_st.starts++;
    g_st.running = 1;
    return TRUE;
}

void IpodPcm_Stop(void)
{
    if (!g_st.running)
        return;
    ChannelOff();
    RI2S(I2S_TXCOM) = I2STXCOM_STOP;
    g_st.running = 0;
    g_st.stops++;
    g_st.lastPeak = g_st.peak;
    g_st.peak = 0;
}

BOOL IpodPcm_PageFinished(DWORD page)
{
    DWORD now, gap, src;

    if (!g_st.running)
        return FALSE;

    now = GetTickCount();
    gap = now - g_lastPoll;
    g_lastPoll = now;
    if (gap > g_st.maxGapMs)
        g_st.maxGapMs = gap;

    if (RDMA(DMAC_RAWINTERR) & (1 << DMAC_CH))
    {
        g_st.dmaErrors++;
        RDMA(DMAC_INTERRCLR) = 1 << DMAC_CH;
    }

    src = RDMA(CH_SRCADDR);
    if (src != g_lastSrc)
    {
        g_lastSrc = src;
        g_lastMove = now;
        g_stalled = FALSE;
    }
    else if (!g_stalled && now - g_lastMove >= STALL_MS)
    {
        g_st.stalls++;
        g_stalled = TRUE;
    }

    return PlayingPage(src) != page;
}

BOOL IpodPcm_StatusIoctl(PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    IPODPCM_STATUS st;
    BOOL rc = FALSE;

    if (pBufOut == NULL || dwLenOut < sizeof(IPODPCM_STATUS))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (g_pClk != NULL && g_pI2s != NULL && g_pDmac != NULL)
    {
        g_st.pwrcon0 = RCLK(CLK_PWRCON0);
        g_st.pwrcon1 = RCLK(CLK_PWRCON1);
        g_st.clkcon3 = RCLK(CLK_CLKCON3);
        g_st.i2sTxCon = RI2S(I2S_TXCON);
        g_st.i2sTxCom = RI2S(I2S_TXCOM);
        g_st.i2sStatus = RI2S(I2S_STATUS);
        g_st.dmacConfig = RDMA(DMAC_CONFIG);
        g_st.position = RDMA(CH_SRCADDR);
        g_st.chConfig = RDMA(CH_CONFIG);
        g_st.chControl = RDMA(CH_CONTROL);
    }
    st = g_st;
    __try
    {
        memcpy(pBufOut, &st, sizeof(st));
        if (pdwActualOut != NULL)
            *pdwActualOut = sizeof(st);
        rc = TRUE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        rc = FALSE;
    }
    return rc;
}
