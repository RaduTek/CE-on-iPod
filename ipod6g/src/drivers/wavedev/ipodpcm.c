//
// ipodpcm.c - the iPod's audio path: the hardware layer of the Windows CE
// sound driver (pcmring.h), under the PDD (wavepdd.cpp)
//
// The path is the one Rockbox plays music through (first tried under
// Windows CE with a plain tone): DMA controller 0, channel 2 -> I2S 0
// -> CS42L55 codec, 24-bit samples in 32-bit words.
//
// At start-up the driver sets the path up itself, the same whether Rockbox
// ran before (ROLO) or not (cold start: the bootloader never touches the
// codec, which stays in reset). The steps are Rockbox's (pcm-s5l8702.c
// sink_dma_init and sink_set_freq, cscodec-6g.c, cs42l55.c audiohw_preinit
// and audiohw_postinit):
//   - I2S 0: its clock on, transmitter 24 bits in 32, I2SCLKDIV 272;
//   - the codec's master clock (CLKCON3, low half: OSC0, 12 MHz);
//   - the codec's reset (port 3 pin 3) low for 10 ms, then high;
//   - through the I2C driver "I2C1:": the codec's version register read
//     (it answers), Rockbox's register sequence (clock master, 44.1 kHz,
//     powered, outputs muted), then headphones at IPODPCM_HP_DB, unmuted.
// The I2S pins are not touched: Rockbox's GPIO table, which the bootloader
// applies too, sets them up.
// Not done: the codec's supply (Rockbox does not know which power chip
// output it is either; it is assumed on), headphone detection, recording.
//
// Playback is a DMA ring with an interrupt per block (pcmring.h; verified
// with the test tone: 168 interrupts in 1004 ms for 1 s of sound).
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <i2c.h>

#include <ipodpcm.h>
#include <ipodi2c.h>
#include "pcmring.h"

// physical addresses (Rockbox firmware/export/s5l87xx.h, S5L8702)
#define PA_CLK              0x3C500000
#define PA_I2S              0x3CA00000
#define PA_DMAC0            0x38200000
#define PA_GPIO             0x3CF00000

#define GPIO_PDAT3          0x64        // bit 3: the codec's reset (0 = in reset)
#define PDAT3_CODEC_RESET   (1 << 3)

#define CLK_CLKCON3         0x0C        // low half: the codec's master clock (0 = OSC0, bit 15 = off)
#define CLKCON3_LOW_MASK    0x0000FFFF
#define CLKCON3_LOW_OFF     0x00008000
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
#define I2S_DIV             (I2S_MCLK_HZ / IPODPCM_RATE)    // 272

#define DMAC_INTSTATUS      0x00        // per channel: TC or error, after the channel masks
#define DMAC_INTTCSTATUS    0x04
#define DMAC_INTTCCLR       0x08
#define DMAC_INTERRCLR      0x10
#define DMAC_RAWINTTC       0x14
#define DMAC_RAWINTERR      0x18
#define DMAC_ENBLDCHNS      0x1C
#define DMAC_CONFIG         0x30
#define DMAC_CH             2           // the channel Rockbox plays through
#define DMAC_CHBASE         (0x100 + (DMAC_CH << 5))
#define CH_SRCADDR          (DMAC_CHBASE + 0x00)
#define CH_DESTADDR         (DMAC_CHBASE + 0x04)
#define CH_LINK             (DMAC_CHBASE + 0x08)
#define CH_CONTROL          (DMAC_CHBASE + 0x0C)
#define CH_CONFIG           (DMAC_CHBASE + 0x10)
#define CHCONFIG_E          (1 << 0)
#define CHCONFIG_IE         (1 << 14)   // error interrupt on (PL080 "mask" bits: 1 = on)
#define CHCONFIG_ITC        (1 << 15)   // terminal count interrupt on
#define CHCONFIG_A          (1 << 17)
#define CH_CONTROL_I        (1u << 31)  // in a list item: terminal count interrupt when it is done
#define DMAC_PERI_I2S0_TX   0x0A

// one list item: 32-bit words, the source moves, the destination is fixed,
// bursts of 8 in and 4 out (the ring adds the interrupt bit, CH_CONTROL_I)
#define CH_CONTROL_VALUE(words) ((words) | (2 << 12) | (1 << 15) | (2 << 18) | (2 << 21) \
                                 | (1 << 26) | (7 << 28))
// memory to peripheral, the DMA controller counts (the ring adds CHCONFIG_IE/ITC)
#define CH_CONFIG_VALUE     ((DMAC_PERI_I2S0_TX << 6) | (1 << 11))

// CS42L55 registers (Rockbox firmware/export/cs42l55.h)
#define CS_CHIPVERSION      0x01
#define CS_HPACTL           0x1A
#define CS_HPBCTL           0x1B
#define CS_HP_LEVEL         ((BYTE)((IPODPCM_HP_DB) & 0x7F))   // dB, two's complement, 7 bits
#define CS_HP_MIN_DB        (-60)       // the codec's range (Rockbox: -60 .. +12 dB, 1 dB steps)
#define CS_HP_MAX_DB        12
#define CS_HP_MUTE          0x80        // HPxCTL bit 7

typedef struct {
    BYTE    reg;
    BYTE    value;
} CS_WRITE;

// Rockbox's audiohw_preinit, in its order
static const CS_WRITE g_csPreinit[] = {
    { 0x00, 0x99 },     // HIDDENCTL: unlock
    { 0x2E, 0x30 },     // hidden registers, Rockbox's (and Apple's) values
    { 0x32, 0x07 },
    { 0x33, 0xFF },
    { 0x34, 0xF8 },
    { 0x35, 0xDC },
    { 0x36, 0xFC },
    { 0x37, 0xAC },
    { 0x3A, 0xF8 },
    { 0x3C, 0xD3 },
    { 0x3D, 0x23 },
    { 0x3E, 0x81 },
    { 0x3F, 0x46 },
    { 0x00, 0x00 },     // HIDDENCTL: lock
    { 0x03, 0xAF },     // PWRCTL2: line outputs always off, headphones never off
    { 0x04, 0x2E },     // CLKCTL1: clock master, SCLK/MCLK before, MCLK / 2
    { 0x05, 0x0B },     // CLKCTL2: 44.1 kHz
    { 0x07, 0x1C },     // MISCCTL: undocumented bit 4, analog zero cross, digital soft ramp
    { 0x02, 0x0F },     // PWRCTL1: charge pump, ADCs and codec powered down
    { 0x0F, 0x83 },     // PLAYCTL: DSP off, master A and B muted
    { 0x0B, 0x00 },     // PGAACTL
    { 0x0C, 0x00 },     // PGABCTL
    { 0x1A, 0x80 },     // HPACTL: muted
    { 0x1B, 0x80 },     // HPBCTL: muted
    { 0x1C, 0x80 },     // LINEACTL: muted
    { 0x1D, 0x80 },     // LINEBCTL: muted
    { 0x02, 0x0E },     // PWRCTL1: codec powered up (charge pump and ADCs still down)
};

// Rockbox's audiohw_postinit (headphones at our level, written before
// these, instead of 0 dB followed by the user's volume)
static const CS_WRITE g_csPostinit[] = {
    { 0x1C, 0x00 },     // LINEACTL: 0 dB, unmuted (the line outputs stay powered down)
    { 0x1D, 0x00 },     // LINEBCTL
    { 0x06, 0x30 },     // CLSHCTL: adaptive power from the signal
    { 0x0F, 0x80 },     // PLAYCTL: master A and B unmuted, DSP still off
};


#define IDLE_WAIT_LOOPS     200000

static volatile UINT32 *g_pClk = NULL, *g_pI2s = NULL, *g_pDmac = NULL, *g_pGpio = NULL;
#define RCLK(off)           g_pClk[(off) >> 2]
#define RI2S(off)           g_pI2s[(off) >> 2]
#define RDMA(off)           g_pDmac[(off) >> 2]
#define RGPIO(off)          g_pGpio[(off) >> 2]

static IPODPCM_STATUS g_st;

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

// One codec register (the I2C handle is set to the codec). FALSE = failed,
// noted in g_st.
static BOOL CsWrite(HANDLE hBus, BYTE reg, BYTE value)
{
    if (!I2cWrite(hBus, reg, &value, 1))
    {
        g_st.codecStep = IPODPCM_CODEC_WRITE;
        g_st.codecReg = reg;
        g_st.codecError = GetLastError();
        return FALSE;
    }
    g_st.codecWrites++;
    return TRUE;
}

static BOOL CsWriteList(HANDLE hBus, const CS_WRITE *pList, DWORD n)
{
    DWORD i;

    for (i = 0; i < n; i++)
    {
        if (!CsWrite(hBus, pList[i].reg, pList[i].value))
            return FALSE;
    }
    return TRUE;
}

// The codec's clock and reset, then its registers. FALSE = failed (g_st.codecStep).
static BOOL CodecSetup(void)
{
    HANDLE hBus;
    BYTE ver = 0;
    BOOL ok;

    // the master clock: off, a moment, on (Rockbox sink_set_freq)
    RCLK(CLK_CLKCON3) = (RCLK(CLK_CLKCON3) & ~CLKCON3_LOW_MASK) | CLKCON3_LOW_OFF;
    Sleep(1);
    RCLK(CLK_CLKCON3) &= ~CLKCON3_LOW_OFF;

    // reset: low 10 ms, high (Rockbox audiohw_preinit)
    RGPIO(GPIO_PDAT3) &= ~PDAT3_CODEC_RESET;
    Sleep(10);
    RGPIO(GPIO_PDAT3) |= PDAT3_CODEC_RESET;
    Sleep(1);

    hBus = I2cOpen(IPODI2C_DEVICE);
    if (hBus == INVALID_HANDLE_VALUE)
    {
        g_st.codecStep = IPODPCM_CODEC_OPEN;
        g_st.codecError = GetLastError();
        return FALSE;
    }
    if (!I2cSetSlaveAddress(hBus, IPODI2C_ADDR_CODEC))
    {
        g_st.codecStep = IPODPCM_CODEC_ADDRESS;
        g_st.codecError = GetLastError();
        I2cClose(hBus);
        return FALSE;
    }
    if (!I2cRead(hBus, CS_CHIPVERSION, &ver, 1))
    {
        g_st.codecStep = IPODPCM_CODEC_ID;
        g_st.codecReg = CS_CHIPVERSION;
        g_st.codecError = GetLastError();
        I2cClose(hBus);
        return FALSE;
    }
    g_st.codecVersion = ver;

    ok = CsWriteList(hBus, g_csPreinit, sizeof(g_csPreinit) / sizeof(g_csPreinit[0]));
    if (ok)
    {
        Sleep(20);                      // Rockbox unmutes a little later too
        g_st.hpReg = CS_HP_LEVEL;
        ok = CsWrite(hBus, CS_HPACTL, CS_HP_LEVEL)
          && CsWrite(hBus, CS_HPBCTL, CS_HP_LEVEL)
          && CsWriteList(hBus, g_csPostinit, sizeof(g_csPostinit) / sizeof(g_csPostinit[0]));
    }
    I2cClose(hBus);
    return ok;
}

// ---------------------------------------------------------------------------
// Windows' volume as the codec's headphone level (IpodPcm_SetVolume)
//
// A volume word v (1 .. 0xFFFF) is 20 * log10(v / 65535) dB, rounded to the
// nearest dB: g_halfDb[n] = 65535 * 10^(-(n + 0.5) / 20), so the attenuation
// is the number of entries above v. Full volume is IPODPCM_HP_DB, the level
// the driver always used; the codec goes down to -60 dB; 0 mutes.
//
static const WORD g_halfDb[72] = {
    61869, 55141, 49144, 43800, 39037, 34792, 31008, 27636, 24631, 21952,
    19565, 17437, 15541, 13851, 12344, 11002,  9806,  8739,  7789,  6942,
     6187,  5514,  4914,  4380,  3904,  3479,  3101,  2764,  2463,  2195,
     1956,  1744,  1554,  1385,  1234,  1100,   981,   874,   779,   694,
      619,   551,   491,   438,   390,   348,   310,   276,   246,   220,
      196,   174,   155,   139,   123,   110,    98,    87,    78,    69,
       62,    55,    49,    44,    39,    35,    31,    28,    25,    22,
       20,    17
};

static CRITICAL_SECTION g_csVol;        // the I2C handle below, g_hpA/B
static BOOL     g_volInit;
static HANDLE   g_hVolBus = INVALID_HANDLE_VALUE;
static BYTE     g_hpA = CS_HP_LEVEL, g_hpB = CS_HP_LEVEL;   // as CodecSetup left them

static BYTE HpLevel(WORD w)
{
    int n = 0, db;

    if (w == 0)
        return CS_HP_MUTE;
    while (n < (int)(sizeof(g_halfDb) / sizeof(g_halfDb[0])) && w < g_halfDb[n])
        n++;
    db = IPODPCM_HP_DB - n;
    if (db < CS_HP_MIN_DB)
        db = CS_HP_MIN_DB;
    if (db > CS_HP_MAX_DB)
        db = CS_HP_MAX_DB;
    return (BYTE)(db & 0x7F);
}

// One headphone register; the I2C handle is opened on first use and kept.
static BOOL VolWrite(BYTE reg, BYTE value)
{
    if (g_hVolBus == INVALID_HANDLE_VALUE)
    {
        g_hVolBus = I2cOpen(IPODI2C_DEVICE);
        if (g_hVolBus == INVALID_HANDLE_VALUE)
            return FALSE;
        if (!I2cSetSlaveAddress(g_hVolBus, IPODI2C_ADDR_CODEC))
        {
            I2cClose(g_hVolBus);
            g_hVolBus = INVALID_HANDLE_VALUE;
            return FALSE;
        }
    }
    return I2cWrite(g_hVolBus, reg, &value, 1);
}

BOOL IpodPcm_SetVolume(ULONG volume)
{
    BYTE a, b;
    BOOL ok = TRUE;

    if (!g_volInit)
        return FALSE;
    a = HpLevel(LOWORD(volume));        // left: HPA, as Rockbox
    b = HpLevel(HIWORD(volume));

    EnterCriticalSection(&g_csVol);
    if (a != g_hpA)
    {
        if (VolWrite(CS_HPACTL, a))
        {
            g_hpA = a;
            g_st.volWrites++;
        }
        else
            ok = FALSE;
    }
    if (b != g_hpB)
    {
        if (VolWrite(CS_HPBCTL, b))
        {
            g_hpB = b;
            g_st.volWrites++;
        }
        else
            ok = FALSE;
    }
    if (!ok)
    {
        if (g_st.volErrors++ == 0)
            RETAILMSG(1, (L"ipodwav: headphone level write failed, error %u (volume 0x%08x)\r\n",
                          GetLastError(), volume));
    }
    g_st.hpReg = g_hpA | (g_hpB << 8);
    LeaveCriticalSection(&g_csVol);
    return ok;
}


// ---------------------------------------------------------------------------
// The DMA ring (pcmring.h)
//
// Memory, not cached: the list items first, the ring after them, block
// after block, so the ring is contiguous and the channel's source address
// says where in it the controller reads.
//
#define PA_VIC0             0x38E00000  // for PcmRing_Dump only
#define VIC_IRQSTATUS       0x000
#define VIC_RAWINTR         0x008
#define VIC_INTENABLE       0x010
#define IRQ_DMAC0           16

#define RING_LLI_BYTES      16
#define RING_OFFSET         512
#define RING_BLOCK_WORDS    (PCMRING_BLOCK_FRAMES * 2)
#define RING_BLOCK_BYTES    (RING_BLOCK_WORDS * 4)
#define RING_BYTES          (PCMRING_BLOCKS * RING_BLOCK_BYTES)
#define RING_MEM_BYTES      (((RING_OFFSET + RING_BYTES) + 4095) & ~4095)

#if (PCMRING_BLOCKS * RING_LLI_BYTES) > RING_OFFSET
#error the list items overlap the ring
#endif
#if RING_BLOCK_WORDS > 4095
#error a block is too big for one list item
#endif

static struct {
    BYTE   *pMem;
    ULONG   memPhys;
    INT32  *pRing;
    DWORD   ringPhys;
    DWORD   playing;            // block the controller is playing
    DWORD   played;             // absolute frame where that block starts
    BOOL    running;
    PCMRING_COUNTERS c;
} g_ring;

static volatile UINT32 *g_pVic0 = NULL;

// All 8 channels of DMA controller 0 share one interrupt line (IRQ 16), and
// no other driver of this BSP uses the controller. Rockbox (also its
// bootloader, so on a cold start too) leaves its LCD DMA channel (4) with a
// finished transfer pending and its interrupt enabled: that holds the line
// up, and once the MDD enables IRQ 16 its thread is woken endlessly at
// priority 110 and the boot stops. So: every other channel's interrupts
// off, and the pending status of every idle channel cleared. Needs the
// controller's clock (written only when gated, as in PathOn). Returns the
// idle channels that had something pending.
static UINT32 DmacQuiet(void)
{
    UINT32 ch, idle, stale;

    if (RCLK(CLK_PWRCON0) & PWRCON0_DMAC0)
        RCLK(CLK_PWRCON0) &= ~PWRCON0_DMAC0;

    for (ch = 0; ch < 8; ch++)
    {
        if (ch != DMAC_CH)
            RDMA(0x100 + (ch << 5) + 0x10) &= ~(CHCONFIG_IE | CHCONFIG_ITC);
    }
    idle = ~RDMA(DMAC_ENBLDCHNS) & 0xFF;
    if (g_ring.running)
        idle &= ~(1u << DMAC_CH);
    stale = (RDMA(DMAC_RAWINTTC) | RDMA(DMAC_RAWINTERR)) & idle;
    if (stale != 0)
    {
        RDMA(DMAC_INTTCCLR) = stale;
        RDMA(DMAC_INTERRCLR) = stale;
    }
    return stale;
}

UINT32 PcmRing_Quiet(void)
{
    if (g_pDmac == NULL || g_pClk == NULL)
        return 0;
    return DmacQuiet();
}

// The clocks, the DMA controller and the I2S transmitter, ready for the
// channel to be started.
static void PathOn(void)
{
    // clocks for the DMA controller and the I2S block (0 = running). Written
    // only when a gate is closed: the I2C driver switches another bit of
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
}

// The ring's memory and its list items: item i plays block i, interrupts
// when done and links to item i + 1; the last links back to the first.
static BOOL RingSetup(void)
{
    UINT32 *pLli;
    DWORD i;

    g_ring.pMem = (BYTE *)AllocPhysMem(RING_MEM_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0,
                                       &g_ring.memPhys);
    if (g_ring.pMem == NULL)
        return FALSE;
    g_ring.pRing = (INT32 *)(g_ring.pMem + RING_OFFSET);
    g_ring.ringPhys = g_ring.memPhys + RING_OFFSET;

    pLli = (UINT32 *)g_ring.pMem;
    for (i = 0; i < PCMRING_BLOCKS; i++)
    {
        pLli[4 * i + 0] = g_ring.ringPhys + i * RING_BLOCK_BYTES;
        pLli[4 * i + 1] = PA_I2S + I2S_TXDB0;
        pLli[4 * i + 2] = g_ring.memPhys + ((i + 1) % PCMRING_BLOCKS) * RING_LLI_BYTES;
        pLli[4 * i + 3] = CH_CONTROL_VALUE(RING_BLOCK_WORDS) | CH_CONTROL_I;
    }
    memset(g_ring.pRing, 0, RING_BYTES);
    return TRUE;
}

BOOL IpodPcm_Init(void)
{
    DWORD div;
    UINT32 stale;

    memset(&g_st, 0, sizeof(g_st));
    g_st.version = IPODPCM_VERSION;
    g_st.rate = IPODPCM_RATE;
    g_st.blocks = PCMRING_BLOCKS;
    g_st.blockFrames = PCMRING_BLOCK_FRAMES;

    g_pClk = (volatile UINT32 *)MapReg(PA_CLK);
    g_pI2s = (volatile UINT32 *)MapReg(PA_I2S);
    g_pDmac = (volatile UINT32 *)MapReg(PA_DMAC0);
    g_pGpio = (volatile UINT32 *)MapReg(PA_GPIO);
    if (g_pClk == NULL || g_pI2s == NULL || g_pDmac == NULL || g_pGpio == NULL)
    {
        g_st.initRc = 1;
        RETAILMSG(1, (L"ipodwav: cannot map the registers\r\n"));
        return FALSE;
    }

    // I2S 0: its clock (written only when gated: see PathOn), then Apple's
    // and Rockbox's order: interface clock, transmitter, divider
    if (RCLK(CLK_PWRCON1) & PWRCON1_I2S0)
        RCLK(CLK_PWRCON1) &= ~PWRCON1_I2S0;
    g_st.clkDiv = RI2S(I2S_CLKDIV);     // as found, for the log
    RI2S(I2S_CLKCON) = 1;
    RI2S(I2S_TXCON) = I2STXCON_24IN32;
    RI2S(I2S_CLKDIV) = I2S_DIV;
    div = I2S_DIV;

    if (!CodecSetup())
    {
        g_st.initRc = 4;
        RETAILMSG(1, (L"ipodwav: codec set-up failed: step %u, register 0x%02x, error %u (I2SCLKDIV was %u)\r\n",
                      g_st.codecStep, g_st.codecReg, g_st.codecError, g_st.clkDiv));
        g_st.clkDiv = div;
        return FALSE;
    }
    RETAILMSG(1, (L"ipodwav: codec set up: version 0x%02x, %u registers, headphones 0x%02x (%d dB), I2SCLKDIV was %u\r\n",
                  g_st.codecVersion, g_st.codecWrites, g_st.hpReg, IPODPCM_HP_DB, g_st.clkDiv));
    g_st.clkDiv = div;

    if (!RingSetup())
    {
        g_st.initRc = 5;
        RETAILMSG(1, (L"ipodwav: no memory for the DMA ring (%u bytes)\r\n", RING_MEM_BYTES));
        return FALSE;
    }
    g_st.bufPhys = g_ring.ringPhys;

    // before anyone enables IRQ 16 (the MDD, right after PDD_AudioInitialize)
    stale = DmacQuiet();                // also the controller's clock on
    ChannelOff();                       // our channel too (Rockbox plays through it)

    InitializeCriticalSection(&g_csVol);
    g_volInit = TRUE;
    g_st.hpReg = g_hpA | (g_hpB << 8);

    g_st.rate = (I2S_MCLK_HZ + div / 2) / div;
    g_st.ready = 1;
    RETAILMSG(1, (L"ipodwav: ready, I2SCLKDIV %u = %u Hz, DMA ring of %u x %u frames at physical 0x%08x; DMA controller 0 quieted (pending on idle channels: 0x%02x)\r\n",
                  div, g_st.rate, PCMRING_BLOCKS, PCMRING_BLOCK_FRAMES, g_ring.ringPhys, stale));
    return TRUE;
}

DWORD IpodPcm_Rate(void)
{
    return g_st.rate ? g_st.rate : IPODPCM_RATE;
}

IPODPCM_STATUS *IpodPcm_Status(void)
{
    return &g_st;
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
        if (g_ring.pMem != NULL)
            g_st.readPos = PcmRing_ReadPos();
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

void PcmRing_Reset(void)
{
    if (g_ring.pMem == NULL || g_ring.running)
        return;
    memset(g_ring.pRing, 0, RING_BYTES);
    g_ring.playing = 0;
    g_ring.played = 0;
}

BOOL PcmRing_Start(void)
{
    const UINT32 *pLli = (const UINT32 *)g_ring.pMem;
    UINT32 stale;

    if (!g_st.ready)
        return FALSE;
    if (g_ring.running)
        return TRUE;

    PathOn();

    // normally nothing left since IpodPcm_Init (see DmacQuiet)
    stale = DmacQuiet();
    if (stale != 0)
        RETAILMSG(1, (L"ipodwav: DMA controller 0: cleared old interrupts of idle channels 0x%02x\r\n",
                      stale));

    // block 0, absolute frame 0 (PcmRing_Reset)
    g_ring.playing = 0;
    g_ring.played = 0;
    memset(&g_ring.c, 0, sizeof(g_ring.c));

    // the channel starts with the first list item, its interrupts on
    RDMA(CH_SRCADDR) = pLli[0];
    RDMA(CH_DESTADDR) = pLli[1];
    RDMA(CH_LINK) = pLli[2];
    RDMA(CH_CONTROL) = pLli[3];
    RDMA(CH_CONFIG) = CH_CONFIG_VALUE | CHCONFIG_IE | CHCONFIG_ITC;
    RDMA(CH_CONFIG) = CH_CONFIG_VALUE | CHCONFIG_IE | CHCONFIG_ITC | CHCONFIG_E;

    g_ring.running = TRUE;
    g_st.running = 1;
    g_st.starts++;
    return TRUE;
}

void PcmRing_Stop(void)
{
    if (!g_ring.running)
        return;
    g_ring.played = PcmRing_ReadPos();
    ChannelOff();                       // also clears the channel's interrupts
    RI2S(I2S_TXCOM) = I2STXCOM_STOP;
    g_ring.running = FALSE;
    g_st.running = 0;
    g_st.stops++;
    g_st.lastPeak = g_st.peak;
    g_st.peak = 0;
}

BOOL PcmRing_Running(void)
{
    return g_ring.running;
}

// The block the controller reads, from its source address. At the very end
// of the ring (source = ring end) block 0 is next.
static DWORD RingBlockNow(void)
{
    DWORD off = RDMA(CH_SRCADDR) - g_ring.ringPhys;

    if (off > RING_BYTES)
        return g_ring.playing;          // not in the ring: no news
    return (off / RING_BLOCK_BYTES) % PCMRING_BLOCKS;
}

int PcmRing_Ack(void)
{
    DWORD cur, done, i;

    if (!g_ring.running)
        return -1;
    g_ring.c.interrupts++;
    g_st.interrupts++;

    if (RDMA(DMAC_INTSTATUS) & ~(1u << DMAC_CH))
        g_ring.c.otherChannels++;

    if (RDMA(DMAC_RAWINTERR) & (1 << DMAC_CH))
    {
        g_ring.c.dmaErrors++;
        g_st.dmaErrors++;
        RETAILMSG(1, (L"ipodwav: DMA error on the ring, stopped\r\n"));
        PcmRing_Dump(L"error");
        PcmRing_Stop();
        return -1;
    }

    // clear first, then look: a block that ends after the look raises a
    // new interrupt
    RDMA(DMAC_INTTCCLR) = 1 << DMAC_CH;

    // every block from the one last seen playing up to (not including) the
    // one playing now has been read: silence it
    cur = RingBlockNow();
    done = (cur + PCMRING_BLOCKS - g_ring.playing) % PCMRING_BLOCKS;
    for (i = 0; i < done; i++)
        memset(g_ring.pRing + ((g_ring.playing + i) % PCMRING_BLOCKS) * RING_BLOCK_WORDS,
               0, RING_BLOCK_BYTES);
    g_ring.playing = cur;
    g_ring.played += done * PCMRING_BLOCK_FRAMES;

    if (done >= 2)
    {
        g_ring.c.late++;
        g_st.late++;
    }
    else if (done == 0)
    {
        g_ring.c.zeroAcks++;
        g_st.zeroAcks++;
    }
    return (int)done;
}

DWORD PcmRing_ReadPos(void)
{
    DWORD off, idx;

    if (!g_ring.running)
        return g_ring.played;

    off = RDMA(CH_SRCADDR) - g_ring.ringPhys;
    if (off > RING_BYTES)
        return g_ring.played + PCMRING_BLOCK_FRAMES;   // between list items: assume the block done
    idx = (off / 8) % PCMRING_FRAMES;
    return g_ring.played
         + (idx + PCMRING_FRAMES - g_ring.playing * PCMRING_BLOCK_FRAMES) % PCMRING_FRAMES;
}

DWORD PcmRing_WindowEnd(void)
{
    return g_ring.played + PCMRING_FRAMES;
}

INT32 *PcmRing_FramePtr(DWORD dwFrame)
{
    return g_ring.pRing + 2 * (dwFrame % PCMRING_FRAMES);
}

DWORD PcmRing_Contiguous(DWORD dwFrame)
{
    return PCMRING_FRAMES - (dwFrame % PCMRING_FRAMES);
}

void PcmRing_Counters(PCMRING_COUNTERS *pc)
{
    if (pc != NULL)
        *pc = g_ring.c;
}

void PcmRing_Dump(LPCWSTR pszWhen)
{
    if (g_pDmac == NULL)
        return;
    if (g_pVic0 == NULL)
        g_pVic0 = (volatile UINT32 *)MapReg(PA_VIC0);

    RETAILMSG(1, (L"ipodwav: [%s] DMAC config 0x%08x int 0x%02x tc 0x%02x rawtc 0x%02x rawerr 0x%02x enabled 0x%02x; ch src 0x%08x link 0x%08x control 0x%08x config 0x%08x\r\n",
                  pszWhen, RDMA(DMAC_CONFIG), RDMA(DMAC_INTSTATUS), RDMA(DMAC_INTTCSTATUS),
                  RDMA(DMAC_RAWINTTC), RDMA(DMAC_RAWINTERR), RDMA(DMAC_ENBLDCHNS),
                  RDMA(CH_SRCADDR), RDMA(CH_LINK), RDMA(CH_CONTROL), RDMA(CH_CONFIG)));
    if (g_pVic0 != NULL)
        RETAILMSG(1, (L"ipodwav: [%s] VIC0 raw 0x%08x enabled 0x%08x irq 0x%08x (DMAC0 = bit %u); PWRCON0 0x%08x I2S TXCOM 0x%08x STATUS 0x%08x\r\n",
                      pszWhen, g_pVic0[VIC_RAWINTR >> 2], g_pVic0[VIC_INTENABLE >> 2],
                      g_pVic0[VIC_IRQSTATUS >> 2], IRQ_DMAC0, RCLK(CLK_PWRCON0),
                      RI2S(I2S_TXCOM), RI2S(I2S_STATUS)));
}
