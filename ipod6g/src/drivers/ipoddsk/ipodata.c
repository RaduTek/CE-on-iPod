//
// ipodata.c - the iPod's disk under Windows CE (ATA, PIO or DMA)
//
// The disk controller of the S5L8702 as Rockbox drives it (Rockbox
// firmware/target/arm/s5l8702/ipod6g/storage_ata-6g.c, the part that is not
// CE-ATA). Rockbox has just loaded Windows CE from this disk, so the disk is
// powered, its pins are set up and the controller is enabled; none of that
// is redone here. No power switching. The only setting sent to the disk is
// its transfer mode (for DMA), and it is reset only after a failed DMA
// transfer.
//
// The commands this file can send, and no others (there is no way to pass
// a command in): IDENTIFY DEVICE (0xEC); READ SECTORS (0x20/0x24 EXT), READ
// DMA (0xC8/0x25 EXT); WRITE SECTORS (0x30/0x34 EXT), WRITE DMA (0xCA/0x35
// EXT); FLUSH CACHE (0xE7/0xEA EXT); STANDBY IMMEDIATE (0xE0, spin down;
// the next command spins the disk up); SET FEATURES (0xEF) subcommand 0x03
// (transfer mode) only; and a software reset through the device control
// register after a failed DMA transfer. Whether writing is allowed at all
// is decided by ipoddsk.c (registry AllowWrite), which calls IpodAta_Write
// only then.
//
// DMA. The controller is a bus master of its own (not the PL080s): it is
// given the physical address and size of a buffer, moves the data itself
// and then sets bit 0 of ATA_IRQ. The buffer here is 64 KB of uncached,
// physically contiguous memory; data is copied into and out of it. The mode (UDMA
// up to 4, else multiword DMA up to 2) and the controller's timing for it
// follow Rockbox's ata_power_up(); its timing values are HClk cycles worked
// out for 108 MHz, so at a slower clock they only get longer. Completion
// (ATA_IRQ bit 0) wakes the reader through the ATA interrupt (SYSINTR_ATA,
// IRQ 29) once IpodAta_UseInterrupt has set it up, else it is polled. The
// controller's interrupt mask is set only while a transfer runs, and
// ATA_IRQ is cleared before InterruptDone unmasks the line again. A DMA
// transfer that fails resets the disk and is done again in PIO (a sector
// write is safe to repeat).
//
// Every register of the disk itself is reached through the controller: a
// write when the controller says it can take one, and for a read a dummy
// read of the register, a wait, and the value from ATA_PIO_RDATA. All waits
// are bounded, by count and by time.
//
// Not reentrant: the caller serialises (ipoddsk.c holds a lock).
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <ipodata.h>

// physical addresses (Rockbox firmware/export/s5l87xx.h, S5L8702)
#define PA_CLK              0x3C500000
#define PA_GPIO             0x3CF00000
#define PA_ATA              0x38700000

#define CLK_PWRCON0         0x48        // bit 5 = disk controller clock gate
#define PWRCON0_ATA         (1 << 5)
#define GPIO_PCON(n)        ((n) << 5)
#define GPIO_PDAT(n)        (((n) << 5) + 4)

#define ATA_CONTROL         0x00
#define ATA_COMMAND         0x08
#define ATA_IRQ             0x10
#define ATA_IRQ_MASK        0x14
#define ATA_CFG             0x18
#define ATA_MDMA_TIME       0x28
#define ATA_PIO_TIME        0x2C
#define ATA_UDMA_TIME       0x30
#define ATA_XFR_NUM         0x34        // bytes to move, minus 1
#define ATA_TBUF_START      0x3C        // where a read puts the data (physical)
#define ATA_TBUF_SIZE       0x40
#define ATA_SBUF_START      0x44        // where a write takes the data from (physical)
#define ATA_SBUF_SIZE       0x48
#define ATA_PIO_DTR         0x54        // the disk's data register
#define ATA_PIO_FED         0x58        // error
#define ATA_PIO_SCR         0x5C        // sector count
#define ATA_PIO_LLR         0x60        // LBA low
#define ATA_PIO_LMR         0x64        // LBA middle
#define ATA_PIO_LHR         0x68        // LBA high
#define ATA_PIO_DVR         0x6C        // device
#define ATA_PIO_CSD         0x70        // command / status
#define ATA_PIO_DAD         0x74        // alternate status
#define ATA_PIO_READY       0x78        // bit 1: a register access may start; bit 0: read data is there
#define ATA_PIO_RDATA       0x7C

#define ATA_COMMAND_START   (1 << 0)
#define ATA_COMMAND_STOP    (1 << 1)
#define ATA_CFG_PIO         (1 << 6)    // the configuration Rockbox reads the identify data in
#define ATA_CFG_WRITE       (1 << 4)    // DMA direction: 1 = to the disk
#define ATA_CFG_CLEAR_START ((1 << 7) | (1 << 8))               // cleared when a DMA transfer starts
#define ATA_CFG_CLEAR_END   ((1 << 2) | (1 << 3) | (1 << 12))   // cleared when it ends
#define ATA_CFG_UDMA        ((1 << 2) | (1 << 3) | (1 << 9) | (1 << 10))
#define ATA_CFG_MWDMA       ((1 << 3) | (1 << 10))
#define ATA_IRQ_XFER_DONE   (1 << 0)
#define ATA_IRQ_ALL         0x1F

#define DAD_NIEN            (1 << 1)    // device control: no interrupts from the disk
#define DAD_SRST            (1 << 2)    // device control: software reset

#define CMD_READ_SECTORS        0x20
#define CMD_READ_SECTORS_EXT    0x24
#define CMD_READ_DMA_EXT        0x25
#define CMD_READ_DMA            0xC8
#define CMD_WRITE_SECTORS       0x30
#define CMD_WRITE_SECTORS_EXT   0x34
#define CMD_WRITE_DMA_EXT       0x35
#define CMD_WRITE_DMA           0xCA
#define CMD_FLUSH_CACHE         0xE7
#define CMD_FLUSH_CACHE_EXT     0xEA
#define CMD_STANDBY_IMMEDIATE   0xE0
#define CMD_IDENTIFY            0xEC
#define CMD_SET_FEATURES        0xEF
#define FEATURE_TRANSFER_MODE   0x03

#define ST_ERR              0x01
#define ST_DRQ              0x08
#define ST_DF               0x20
#define ST_DRDY             0x40
#define ST_BSY              0x80

#define READY_LOOPS         200000      // per register access
#define SPIN_LOOPS          50          // short waits are spun, longer ones sleep
#define IDENTIFY_WAIT_MS    5000
// The waits of a transfer; g_spinUpMs is added to all of them for the first
// transfer after IpodAta_Standby (the disk spins up before it answers).
#define READ_WAIT_MS        (2000 + g_spinUpMs)
#define WRITE_WAIT_MS       (5000 + g_spinUpMs)
#define DMA_WAIT_MS         (5000 + g_spinUpMs)
#define SPINUP_WAIT_MS      15000
#define FLUSH_WAIT_MS       10000       // a disk may write back several MB of cache
#define RESET_WAIT_MS       5000
#define DMA_BUF_BYTES       (IPODATA_MAX_SECTORS * IPODATA_SECTOR_BYTES)

// The PC test redirects these into its model of the controller and the disk.
#ifndef ATA_RD
static volatile UINT32 *g_pAta = NULL, *g_pClk = NULL, *g_pGpio = NULL;
#define ATA_RD(off)         (g_pAta[(off) >> 2])
#define ATA_WR(off, val)    (g_pAta[(off) >> 2] = (val))
#define CLK_RD(off)         (g_pClk[(off) >> 2])
#define GPIO_RD(off)        (g_pGpio[(off) >> 2])
#define IPODATA_MAP_REGISTERS
#endif

static IPODATA_STATUS g_st;
static WORD g_ident[256];
static BYTE *g_pDmaBuf = NULL;          // DMA_BUF_BYTES, uncached; physical address in g_st.dmaBufPhys
static DWORD g_dmaCfg = 0;              // ATA_CFG bits for the chosen DMA mode, 0 = PIO
static HANDLE g_hIntr = NULL;           // event of SYSINTR g_st.sysIntr, NULL = polled
static DWORD g_spinUpMs = 0;            // extra wait for this transfer: SPINUP_WAIT_MS after standby

// The controller's timing register for each mode (Rockbox, S5L8702, HClk 108 MHz).
static const DWORD g_udmaTime[5] = { 0x4071152, 0x2050D52, 0x2030A52, 0x1020A52, 0x1010A52 };
static const DWORD g_mdmaTime[3] = { 0x1C175, 0x7083, 0x5072 };

//------------------------------------------------------------------------------
// one access to a register of the disk
//
static BOOL WaitReady(DWORD bit)
{
    int i;

    for (i = 0; i < READY_LOOPS; i++)
    {
        if (ATA_RD(ATA_PIO_READY) & bit)
            return TRUE;
    }
    return FALSE;
}

static BOOL DiskRead(DWORD reg, DWORD *pVal)
{
    volatile DWORD dummy;

    if (!WaitReady(2))
        return FALSE;
    dummy = ATA_RD(reg);                // this read starts the access
    (void)dummy;
    if (!WaitReady(1))
        return FALSE;
    *pVal = ATA_RD(ATA_PIO_RDATA);
    return TRUE;
}

static BOOL DiskWrite(DWORD reg, DWORD val)
{
    if (!WaitReady(2))
        return FALSE;
    ATA_WR(reg, val);
    return TRUE;
}

static DWORD Fail(DWORD step, DWORD code)
{
    g_st.lastStep = step;
    g_st.lastCode = code;
    g_st.errors++;
    return code;
}

//------------------------------------------------------------------------------
// waits; each returns IPODATA_OK or an error code and leaves the status byte
// it saw in g_st.lastStatus
//
static DWORD WaitNotBusy(DWORD timeoutMs)
{
    DWORD start = GetTickCount(), v, loops = 0;

    for (;;)
    {
        if (!DiskRead(ATA_PIO_CSD, &v))
            return IPODATA_E_HANDSHAKE;
        g_st.lastStatus = v & 0xFF;
        if (!(v & ST_BSY))
            return IPODATA_OK;
        if (GetTickCount() - start >= timeoutMs || loops >= timeoutMs + SPIN_LOOPS)
            return IPODATA_E_BUSY;
        if (++loops > SPIN_LOOPS)
            Sleep(1);
    }
}

static DWORD WaitReadyForCommand(DWORD timeoutMs)
{
    DWORD start = GetTickCount(), v, loops = 0, rc;

    rc = WaitNotBusy(timeoutMs);
    if (rc != IPODATA_OK)
        return rc;
    for (;;)
    {
        if (!DiskRead(ATA_PIO_DAD, &v))
            return IPODATA_E_HANDSHAKE;
        g_st.lastStatus = v & 0xFF;
        if (v & ST_DRDY)
            return IPODATA_OK;
        if (GetTickCount() - start >= timeoutMs || loops >= timeoutMs + SPIN_LOOPS)
            return IPODATA_E_NOT_READY;
        if (++loops > SPIN_LOOPS)
            Sleep(1);
    }
}

static DWORD NoteDeviceError(void)
{
    DWORD e = 0;

    if (DiskRead(ATA_PIO_FED, &e))
        g_st.lastError = e & 0xFF;
    return IPODATA_E_DEVICE;
}

static DWORD WaitForData(DWORD timeoutMs)
{
    DWORD start = GetTickCount(), v, loops = 0, rc;

    rc = WaitNotBusy(timeoutMs);
    if (rc != IPODATA_OK)
        return rc;
    for (;;)
    {
        if (!DiskRead(ATA_PIO_DAD, &v))
            return IPODATA_E_HANDSHAKE;
        g_st.lastStatus = v & 0xFF;
        if (v & ST_ERR)
            return NoteDeviceError();
        if ((v & (ST_BSY | ST_DRQ)) == ST_DRQ)
            return IPODATA_OK;
        if (GetTickCount() - start >= timeoutMs || loops >= timeoutMs + SPIN_LOOPS)
            return IPODATA_E_NO_DATA;
        if (++loops > SPIN_LOOPS)
            Sleep(1);
    }
}

static DWORD WaitForEnd(DWORD timeoutMs)
{
    DWORD v, rc;

    rc = WaitNotBusy(timeoutMs);
    if (rc != IPODATA_OK)
        return rc;
    if (!DiskRead(ATA_PIO_DAD, &v))
        return IPODATA_E_HANDSHAKE;
    g_st.lastStatus = v & 0xFF;
    if (v & ST_ERR)
        return NoteDeviceError();
    if ((v & (ST_BSY | ST_DRDY | ST_DF | ST_DRQ)) == ST_DRDY)
        return IPODATA_OK;
    return IPODATA_E_END;
}

// One sector's worth of data words, low byte first.
static BOOL TakeWords(BYTE *pOut, DWORD words)
{
    DWORD i, v;

    for (i = 0; i < words; i++)
    {
        if (!DiskRead(ATA_PIO_DTR, &v))
            return FALSE;
        pOut[i * 2] = (BYTE)(v & 0xFF);
        pOut[i * 2 + 1] = (BYTE)((v >> 8) & 0xFF);
    }
    return TRUE;
}

// The same the other way: one sector's worth of data words to the disk.
static BOOL GiveWords(const BYTE *pIn, DWORD words)
{
    DWORD i;

    for (i = 0; i < words; i++)
    {
        if (!DiskWrite(ATA_PIO_DTR, (DWORD)pIn[i * 2] | ((DWORD)pIn[i * 2 + 1] << 8)))
            return FALSE;
    }
    return TRUE;
}

//------------------------------------------------------------------------------
#ifdef IPODATA_MAP_REGISTERS
static void *MapReg(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return MmMapIoSpace(a, 0x1000, FALSE);
}
#endif

BOOL IpodAta_Init(void)
{
    memset(&g_st, 0, sizeof(g_st));
    memset(g_ident, 0, sizeof(g_ident));
    g_st.version = IPODATA_VERSION;

#ifdef IPODATA_MAP_REGISTERS
    g_pClk = (volatile UINT32 *)MapReg(PA_CLK);
    g_pGpio = (volatile UINT32 *)MapReg(PA_GPIO);
    g_pAta = (volatile UINT32 *)MapReg(PA_ATA);
    if (g_pClk == NULL || g_pGpio == NULL || g_pAta == NULL)
    {
        RETAILMSG(1, (L"ipodata: cannot map the registers\r\n"));
        return FALSE;
    }
#endif
    g_st.mapped = 1;

    // what Rockbox left; the controller's own registers only if it has its clock
    g_st.bootPwrcon0 = CLK_RD(CLK_PWRCON0);
    g_st.bootPcon7 = GPIO_RD(GPIO_PCON(7));
    g_st.bootPcon8 = GPIO_RD(GPIO_PCON(8));
    g_st.bootPcon9 = GPIO_RD(GPIO_PCON(9));
    g_st.bootPcon10 = GPIO_RD(GPIO_PCON(10)) & 0xFFFF;
    g_st.bootPdat11 = GPIO_RD(GPIO_PDAT(11));
    if (!(g_st.bootPwrcon0 & PWRCON0_ATA))
    {
        g_st.bootControl = ATA_RD(ATA_CONTROL);
        g_st.bootCfg = ATA_RD(ATA_CFG);
        g_st.bootPioTime = ATA_RD(ATA_PIO_TIME);
        g_st.bootIrq = ATA_RD(ATA_IRQ);
        g_st.bootIrqMask = ATA_RD(ATA_IRQ_MASK);
        g_st.bootMdmaTime = ATA_RD(ATA_MDMA_TIME);
        g_st.bootUdmaTime = ATA_RD(ATA_UDMA_TIME);
    }

#ifdef IPODATA_MAP_REGISTERS
    {
        ULONG phys = 0;

        g_pDmaBuf = (BYTE *)AllocPhysMem(DMA_BUF_BYTES, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &phys);
        if (g_pDmaBuf != NULL)
            g_st.dmaBufPhys = phys;
        else
            RETAILMSG(1, (L"ipodata: no DMA buffer (error %u): reads stay PIO\r\n", GetLastError()));
    }
#endif
    RETAILMSG(1, (L"ipodata: disk pins %08x %08x %08x %04x, controller %x, cfg %x\r\n",
                  g_st.bootPcon7, g_st.bootPcon8, g_st.bootPcon9, g_st.bootPcon10,
                  g_st.bootControl, g_st.bootCfg));
    return TRUE;
}

// Characters of an identify string: two per word, the high byte first.
static void IdentString(char *pOut, int firstWord, int words)
{
    int i, n = 0;

    for (i = 0; i < words; i++)
    {
        pOut[n++] = (char)(g_ident[firstWord + i] >> 8);
        pOut[n++] = (char)(g_ident[firstWord + i] & 0xFF);
    }
    while (n > 0 && (pOut[n - 1] == ' ' || pOut[n - 1] == 0))
        n--;
    pOut[n] = 0;
    for (i = 0; i < n; i++)
    {
        if (pOut[i] < 0x20 || pOut[i] > 0x7E)
            pOut[i] = '?';
    }
}

//------------------------------------------------------------------------------
// the transfer mode
//
// The highest mode up to max whose bit is set; -1 = none.
static int BestMode(DWORD bits, int max)
{
    int n;

    for (n = max; n >= 0; n--)
    {
        if (bits & (1u << n))
            return n;
    }
    return -1;
}

static DWORD SetTransferMode(DWORD mode)
{
    DWORD rc, v;

    rc = WaitReadyForCommand(IDENTIFY_WAIT_MS);
    if (rc != IPODATA_OK)
        return rc;
    if (!DiskWrite(ATA_PIO_DVR, 0) || !DiskWrite(ATA_PIO_FED, FEATURE_TRANSFER_MODE)
        || !DiskWrite(ATA_PIO_SCR, mode) || !DiskWrite(ATA_PIO_CSD, CMD_SET_FEATURES))
        return IPODATA_E_HANDSHAKE;
    rc = WaitNotBusy(IDENTIFY_WAIT_MS);
    if (rc != IPODATA_OK)
        return rc;
    if (!DiskRead(ATA_PIO_DAD, &v))
        return IPODATA_E_HANDSHAKE;
    g_st.lastStatus = v & 0xFF;
    if (v & ST_ERR)
        return NoteDeviceError();
    return IPODATA_OK;
}

// Picks the fastest DMA mode the disk offers (Rockbox's rules), sets the
// controller's timing and tells the disk. Leaves PIO in place when there is
// none or the disk refuses.
static void SetupDma(void)
{
    DWORD mode = IPODATA_MODE_PIO, cfg = 0, rc;
    int n, max;

    g_st.dmaMode = IPODATA_MODE_PIO;
    g_st.modeError = 0;
    g_dmaCfg = 0;
    if (!g_st.dmaWanted || g_pDmaBuf == NULL)
        return;

    if ((g_ident[53] & (1 << 2)) && (g_ident[88] & 0x1F))
    {
        // above UDMA 2 only on an 80-wire bus (word 93 bit 13; word 76 = 0 is PATA)
        max = (g_ident[76] == 0 && !(g_ident[93] & (1 << 13))) ? 2 : 4;
        n = BestMode(g_ident[88], max);
        if (n >= 0)
        {
            mode = IPODATA_MODE_UDMA(n);
            cfg = ATA_CFG_UDMA;
            ATA_WR(ATA_UDMA_TIME, g_udmaTime[n]);
        }
    }
    if (mode == IPODATA_MODE_PIO && (g_ident[63] & 0x07))
    {
        n = BestMode(g_ident[63], 2);
        mode = IPODATA_MODE_MWDMA(n);
        cfg = ATA_CFG_MWDMA;
        ATA_WR(ATA_MDMA_TIME, g_mdmaTime[n]);
    }
    if (mode == IPODATA_MODE_PIO)
        return;

    rc = SetTransferMode(mode);
    if (rc != IPODATA_OK)
    {
        g_st.modeError = rc;
        RETAILMSG(1, (L"ipodata: SET FEATURES mode %02x failed (code %u, status %02x, error %02x): PIO\r\n",
                      mode, rc, g_st.lastStatus, g_st.lastError));
        return;
    }
    g_st.dmaMode = mode;
    g_dmaCfg = cfg;
}

// After a failed transfer: the disk's software reset, then wait for it. The
// transfer mode set before stays (a reset does not undo SET FEATURES unless
// the disk was told to revert to its defaults, which nothing here does).
static void SoftReset(void)
{
    DiskWrite(ATA_PIO_DAD, DAD_NIEN | DAD_SRST);
    Sleep(1);
    DiskWrite(ATA_PIO_DAD, 0);
    Sleep(2);
    WaitReadyForCommand(RESET_WAIT_MS);
}

DWORD IpodAta_Attach(BOOL useDma)
{
    DWORD rc, total;

    g_st.attached = 0;
    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    g_st.lastStatus = 0;
    g_st.lastError = 0;

    // is the disk as Rockbox uses it?
    if (!g_st.mapped)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_NOT_MAPPED);
    if (CLK_RD(CLK_PWRCON0) & PWRCON0_ATA)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_CLOCK_OFF);
    if (GPIO_RD(GPIO_PCON(7)) == 0 && GPIO_RD(GPIO_PCON(8)) == 0 && GPIO_RD(GPIO_PCON(9)) == 0
        && (GPIO_RD(GPIO_PCON(10)) & 0xFFFF) == 0)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_POWERED_DOWN);
    if (GPIO_RD(GPIO_PDAT(11)) & (1 << 1))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_CEATA);
    if (!(ATA_RD(ATA_CONTROL) & 1))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_CTRL_OFF);

    // The state Rockbox reads the identify data in: no transfer running (Rockbox
    // writes this before every transfer), plain PIO configuration.
    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);
    ATA_WR(ATA_CFG, ATA_CFG_PIO);

    g_st.identifies++;
    rc = WaitNotBusy(IDENTIFY_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_ID_IDLE, rc);
    if (!DiskWrite(ATA_PIO_DVR, 0) || !DiskWrite(ATA_PIO_CSD, CMD_IDENTIFY))
        return Fail(IPODATA_STEP_ID_CMD, IPODATA_E_HANDSHAKE);
    rc = WaitForData(IDENTIFY_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_ID_DATA, rc);
    if (!TakeWords((BYTE *)g_ident, 256))
        return Fail(IPODATA_STEP_ID_DATA, IPODATA_E_HANDSHAKE);

    IdentString(g_st.model, 27, 20);
    IdentString(g_st.firmware, 23, 4);
    g_st.word49 = g_ident[49];
    g_st.word83 = g_ident[83];
    g_st.word88 = g_ident[88];
    g_st.word106 = g_ident[106];
    g_st.word53 = g_ident[53];
    g_st.word63 = g_ident[63];
    g_st.word93 = g_ident[93];

    // size and addressing, by Rockbox's rules
    total = ((DWORD)g_ident[61] << 16) | g_ident[60];
    if ((g_ident[83] & (1 << 10)) && total == 0x0FFFFFFF)
    {
        g_st.lba48 = 1;
        g_st.sectorsLow = ((DWORD)g_ident[101] << 16) | g_ident[100];
        g_st.sectorsHigh = ((DWORD)g_ident[103] << 16) | g_ident[102];
    }
    else
    {
        g_st.lba48 = 0;
        g_st.sectorsLow = total;
        g_st.sectorsHigh = 0;
    }
    if ((g_ident[106] & 0xD000) == 0x5000)
        g_st.sectorBytes = (((DWORD)g_ident[118] << 16) | g_ident[117]) * 2;
    else
        g_st.sectorBytes = 512;

    // the flush command, by Rockbox's rules: the EXT one for an LBA48 disk
    // that has it, else FLUSH CACHE where ATA-5 or later or word 83 says so
    g_st.word80 = g_ident[80];
    g_st.word82 = g_ident[82];
    g_st.word85 = g_ident[85];
    if (g_st.lba48 && (g_ident[83] & (1 << 13)))
        g_st.flushCmd = CMD_FLUSH_CACHE_EXT;
    else if ((g_ident[83] & (1 << 12)) || (g_ident[80] != 0xFFFF && g_ident[80] >= (1 << 5)))
        g_st.flushCmd = CMD_FLUSH_CACHE;
    else
        g_st.flushCmd = 0;

    g_st.attached = 1;
    g_st.dmaWanted = useDma ? 1 : 0;
    SetupDma();
    RETAILMSG(1, (L"ipodata: transfer mode %02x (%s), words 53 %04x 63 %04x 88 %04x 93 %04x\r\n",
                  g_st.dmaMode, g_st.dmaMode ? L"DMA" : (useDma ? L"PIO, no DMA" : L"PIO, UseDma 0"),
                  g_ident[53], g_ident[63], g_ident[88], g_ident[93]));
    RETAILMSG(1, (L"ipodata: flush command %02x, write cache %s (words 80 %04x 82 %04x 83 %04x 85 %04x)\r\n",
                  g_st.flushCmd, (g_ident[85] & (1 << 5)) ? L"on" : L"off",
                  g_ident[80], g_ident[82], g_ident[83], g_ident[85]));
    RETAILMSG(1, (L"ipodata: timing at boot MDMA %x UDMA %x, now MDMA %x UDMA %x, buffer at %08x\r\n",
                  g_st.bootMdmaTime, g_st.bootUdmaTime, ATA_RD(ATA_MDMA_TIME), ATA_RD(ATA_UDMA_TIME),
                  g_st.dmaBufPhys));
    // a failed SET FEATURES does not fail the attach: reads stay PIO
    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    return IPODATA_OK;
}

// A DMA transfer for a READ DMA or WRITE DMA command that has been sent:
// g_pDmaBuf holds the data to write, or gets the data read.
static DWORD DmaTransfer(DWORD bytes, BOOL write)
{
    DWORD start, loops = 0, done = 0, rc;

    rc = WaitForData(write ? WRITE_WAIT_MS : READ_WAIT_MS);     // the disk is ready to move the data
    if (rc != IPODATA_OK)
        return rc;

    if (write)
    {
        ATA_WR(ATA_SBUF_START, g_st.dmaBufPhys);
        ATA_WR(ATA_SBUF_SIZE, bytes);
        ATA_WR(ATA_CFG, ATA_RD(ATA_CFG) | ATA_CFG_WRITE);
    }
    else
    {
        ATA_WR(ATA_TBUF_START, g_st.dmaBufPhys);
        ATA_WR(ATA_TBUF_SIZE, bytes);
        ATA_WR(ATA_CFG, ATA_RD(ATA_CFG) & ~ATA_CFG_WRITE);
    }
    ATA_WR(ATA_XFR_NUM, bytes - 1);
    ATA_WR(ATA_CFG, (ATA_RD(ATA_CFG) | g_dmaCfg) & ~ATA_CFG_CLEAR_START);
    ATA_WR(ATA_IRQ, ATA_IRQ_ALL);
    ATA_WR(ATA_IRQ_MASK, ATA_IRQ_XFER_DONE);
    ATA_WR(ATA_COMMAND, ATA_COMMAND_START);

    start = GetTickCount();
    if (g_hIntr != NULL)
    {
        DWORD wr = WaitForSingleObject(g_hIntr, DMA_WAIT_MS);

        if (ATA_RD(ATA_IRQ) & ATA_IRQ_XFER_DONE)
        {
            done = 1;
            if (wr == WAIT_OBJECT_0)
                g_st.intrWakes++;
            else
                g_st.intrMissed++;          // done, but the interrupt never came
        }
        else if (wr == WAIT_OBJECT_0)
        {
            // woken with the transfer not done: the line stays masked (the
            // ISR masked it) and the rest of this transfer is polled below
            g_st.intrSpurious++;
        }
    }
    if (!done && (g_hIntr == NULL || GetTickCount() - start < DMA_WAIT_MS))
    {
        for (;;)
        {
            if (ATA_RD(ATA_IRQ) & ATA_IRQ_XFER_DONE)
            {
                done = 1;
                break;
            }
            if (GetTickCount() - start >= DMA_WAIT_MS || loops >= DMA_WAIT_MS + SPIN_LOOPS)
                break;
            if (++loops > SPIN_LOOPS)
                Sleep(1);
        }
    }

    // stop and tidy up in every case: the controller's source is masked and
    // cleared before the line is unmasked, so it cannot fire again
    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);
    ATA_WR(ATA_CFG, ATA_RD(ATA_CFG) & ~(ATA_CFG_CLEAR_END | ATA_CFG_WRITE));
    ATA_WR(ATA_IRQ_MASK, 0);
    ATA_WR(ATA_IRQ, ATA_IRQ_ALL);
    if (g_hIntr != NULL)
    {
        WaitForSingleObject(g_hIntr, 0);    // an interrupt that came after the wait gave up
        InterruptDone(g_st.sysIntr);
    }
    if (!done)
    {
        g_st.dmaTimeouts++;
        return IPODATA_E_DMA_TIMEOUT;
    }
    return IPODATA_OK;
}

// The command for a transfer of count sectors at lba (taskfile + command).
static BOOL SendTransfer(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE cmd28, BYTE cmd48)
{
    BOOL ok = DiskWrite(ATA_PIO_DVR, 0);

    if (g_st.lba48)
    {
        // the "previous" half of each register first, then the current one
        ok = ok && DiskWrite(ATA_PIO_SCR, (count >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_SCR, count & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaHigh >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, lbaHigh & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, (lbaLow >> 24) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaLow >> 16) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, (lbaLow >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, lbaLow & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_DVR, 1 << 6);
        ok = ok && DiskWrite(ATA_PIO_CSD, cmd48);
    }
    else
    {
        ok = ok && DiskWrite(ATA_PIO_SCR, count & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaLow >> 16) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, (lbaLow >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, lbaLow & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_DVR, (1 << 6) | ((lbaLow >> 24) & 0x0F));
        ok = ok && DiskWrite(ATA_PIO_CSD, cmd28);
    }
    return ok;
}

// One read command, by DMA or PIO. The caller has checked the request.
static DWORD ReadOnce(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE *pOut, BOOL dma)
{
    DWORD rc, i;

    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);          // as Rockbox does before every transfer

    rc = WaitReadyForCommand(READ_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_RD_READY, rc);
    if (!SendTransfer(lbaLow, lbaHigh, count,
                      dma ? CMD_READ_DMA : CMD_READ_SECTORS, dma ? CMD_READ_DMA_EXT : CMD_READ_SECTORS_EXT))
        return Fail(IPODATA_STEP_RD_CMD, IPODATA_E_HANDSHAKE);

    if (dma)
    {
        rc = DmaTransfer(count * IPODATA_SECTOR_BYTES, FALSE);
        if (rc != IPODATA_OK)
            return Fail(IPODATA_STEP_RD_DMA, rc);
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            rc = WaitForData(READ_WAIT_MS);
            if (rc != IPODATA_OK)
                return Fail(IPODATA_STEP_RD_DATA, rc);
            if (!TakeWords(pOut + i * IPODATA_SECTOR_BYTES, IPODATA_SECTOR_BYTES / 2))
                return Fail(IPODATA_STEP_RD_DATA, IPODATA_E_HANDSHAKE);
        }
    }
    rc = WaitForEnd(READ_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_RD_END, rc);

    if (dma)
        memcpy(pOut, g_pDmaBuf, count * IPODATA_SECTOR_BYTES);
    return IPODATA_OK;
}

// One write command, by DMA (the data already in g_pDmaBuf) or PIO.
static DWORD WriteOnce(DWORD lbaLow, DWORD lbaHigh, DWORD count, const BYTE *pIn, BOOL dma)
{
    DWORD rc, i;

    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);

    rc = WaitReadyForCommand(WRITE_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_WR_READY, rc);
    if (!SendTransfer(lbaLow, lbaHigh, count,
                      dma ? CMD_WRITE_DMA : CMD_WRITE_SECTORS, dma ? CMD_WRITE_DMA_EXT : CMD_WRITE_SECTORS_EXT))
        return Fail(IPODATA_STEP_WR_CMD, IPODATA_E_HANDSHAKE);

    if (dma)
    {
        rc = DmaTransfer(count * IPODATA_SECTOR_BYTES, TRUE);
        if (rc != IPODATA_OK)
            return Fail(IPODATA_STEP_WR_DMA, rc);
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            rc = WaitForData(WRITE_WAIT_MS);        // the disk asks for the next sector
            if (rc != IPODATA_OK)
                return Fail(IPODATA_STEP_WR_DATA, rc);
            if (!GiveWords(pIn + i * IPODATA_SECTOR_BYTES, IPODATA_SECTOR_BYTES / 2))
                return Fail(IPODATA_STEP_WR_DATA, IPODATA_E_HANDSHAKE);
        }
    }
    rc = WaitForEnd(WRITE_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_WR_END, rc);
    return IPODATA_OK;
}

// The checks a read or write request must pass. 0 = ok.
static DWORD CheckRequest(DWORD lbaLow, DWORD lbaHigh, DWORD count, const void *pData)
{
    DWORD endLow, endHigh;

    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    g_st.lastStatus = 0;
    g_st.lastError = 0;

    if (!g_st.attached)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_NOT_ATTACHED);
    if (g_st.sectorBytes != IPODATA_SECTOR_BYTES)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_SECTOR_SIZE);
    if (pData == NULL || count == 0)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_PARAM);
    if (count > IPODATA_MAX_SECTORS)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    // lba + count must not pass the end of the disk (64-bit, done in halves)
    endLow = lbaLow + count;
    endHigh = lbaHigh + (endLow < lbaLow ? 1 : 0);
    if (endHigh < lbaHigh || endHigh > g_st.sectorsHigh
        || (endHigh == g_st.sectorsHigh && endLow > g_st.sectorsLow))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    if (!g_st.lba48 && (lbaHigh != 0 || lbaLow > 0x0FFFFFFF || count > 256))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    if (g_st.lba48 && lbaHigh > 0xFFFF)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    return IPODATA_OK;
}

// Before a read or write: extra time for every wait if the disk was spun
// down. The command spins it up whatever comes of it, so the flag goes.
static void SpinUpBegin(void)
{
    g_spinUpMs = g_st.standby ? SPINUP_WAIT_MS : 0;
    g_st.standby = 0;
}

// After it: how long the spin-up took.
static void SpinUpEnd(DWORD rc, DWORD t0)
{
    DWORD ms;

    if (g_spinUpMs == 0)
        return;
    g_spinUpMs = 0;
    ms = GetTickCount() - t0;
    if (rc == IPODATA_OK)
    {
        g_st.spinUps++;
        g_st.lastSpinUpMs = ms;
        if (ms > g_st.maxSpinUpMs)
            g_st.maxSpinUpMs = ms;
    }
    RETAILMSG(1, (L"ipodata: disk spun up for a transfer in %u ms%s\r\n", ms, rc == IPODATA_OK ? L"" : L", FAILED"));
}

DWORD IpodAta_Read(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE *pOut)
{
    DWORD rc, t0;

    rc = CheckRequest(lbaLow, lbaHigh, count, pOut);
    if (rc != IPODATA_OK)
        return rc;

    t0 = GetTickCount();
    g_st.reads++;
    SpinUpBegin();
    if (g_st.dmaMode != IPODATA_MODE_PIO)
    {
        rc = ReadOnce(lbaLow, lbaHigh, count, pOut, TRUE);
        if (rc == IPODATA_OK)
        {
            g_st.dmaReads++;
        }
        else
        {
            // lastStep/lastCode keep the DMA failure for the panel unless the
            // PIO read fails too; the disk is reset and the read done again
            RETAILMSG(1, (L"ipodata: DMA read at %u+%u failed (step %u code %u status %02x error %02x), PIO again\r\n",
                          lbaLow, count, g_st.lastStep, g_st.lastCode, g_st.lastStatus, g_st.lastError));
            SoftReset();
            rc = ReadOnce(lbaLow, lbaHigh, count, pOut, FALSE);
        }
    }
    else
    {
        rc = ReadOnce(lbaLow, lbaHigh, count, pOut, FALSE);
    }
    SpinUpEnd(rc, t0);
    if (rc != IPODATA_OK)
        return rc;

    g_st.sectorsRead += count;
    g_st.lastReadMs = GetTickCount() - t0;
    g_st.lastReadSectors = count;
    return IPODATA_OK;
}

DWORD IpodAta_Write(DWORD lbaLow, DWORD lbaHigh, DWORD count, const BYTE *pIn)
{
    DWORD rc, t0;

    rc = CheckRequest(lbaLow, lbaHigh, count, pIn);
    if (rc != IPODATA_OK)
        return rc;

    t0 = GetTickCount();
    g_st.writes++;
    SpinUpBegin();
    if (g_st.dmaMode != IPODATA_MODE_PIO)
    {
        memcpy(g_pDmaBuf, pIn, count * IPODATA_SECTOR_BYTES);
        rc = WriteOnce(lbaLow, lbaHigh, count, pIn, TRUE);
        if (rc == IPODATA_OK)
        {
            g_st.dmaWrites++;
        }
        else
        {
            // writing the same sectors again is safe
            RETAILMSG(1, (L"ipodata: DMA write at %u+%u failed (step %u code %u status %02x error %02x), PIO again\r\n",
                          lbaLow, count, g_st.lastStep, g_st.lastCode, g_st.lastStatus, g_st.lastError));
            g_st.writeRetries++;
            SoftReset();
            rc = WriteOnce(lbaLow, lbaHigh, count, pIn, FALSE);
        }
    }
    else
    {
        rc = WriteOnce(lbaLow, lbaHigh, count, pIn, FALSE);
    }
    SpinUpEnd(rc, t0);
    if (rc != IPODATA_OK)
    {
        RETAILMSG(1, (L"ipodata: WRITE at %u+%u FAILED (step %u code %u status %02x error %02x)\r\n",
                      lbaLow, count, g_st.lastStep, g_st.lastCode, g_st.lastStatus, g_st.lastError));
        return rc;
    }

    g_st.sectorsWritten += count;
    g_st.lastWriteMs = GetTickCount() - t0;
    return IPODATA_OK;
}

DWORD IpodAta_Flush(void)
{
    DWORD rc, v, t0;

    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    if (!g_st.attached)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_NOT_ATTACHED);
    if (g_st.flushCmd == 0)
        return IPODATA_OK;                  // the disk has no flush command
    if (g_st.standby)
        return IPODATA_OK;                  // standby wrote the cache back; do not spin up for nothing

    t0 = GetTickCount();
    g_st.flushes++;
    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);
    rc = WaitReadyForCommand(FLUSH_WAIT_MS);
    if (rc == IPODATA_OK && (!DiskWrite(ATA_PIO_DVR, 0) || !DiskWrite(ATA_PIO_CSD, g_st.flushCmd)))
        rc = IPODATA_E_HANDSHAKE;
    if (rc == IPODATA_OK)
        rc = WaitNotBusy(FLUSH_WAIT_MS);
    if (rc == IPODATA_OK)
    {
        if (!DiskRead(ATA_PIO_DAD, &v))
            rc = IPODATA_E_HANDSHAKE;
        else if ((g_st.lastStatus = v & 0xFF) & ST_ERR)
            rc = NoteDeviceError();
    }
    g_st.lastFlushMs = GetTickCount() - t0;
    if (rc != IPODATA_OK)
    {
        g_st.flushErrors++;
        Fail(IPODATA_STEP_FLUSH, rc);
        RETAILMSG(1, (L"ipodata: FLUSH CACHE FAILED (code %u status %02x error %02x)\r\n",
                      rc, g_st.lastStatus, g_st.lastError));
    }
    return rc;
}

DWORD IpodAta_Standby(void)
{
    DWORD rc, v, t0;

    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    if (!g_st.attached)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_NOT_ATTACHED);
    if (g_st.standby)
        return IPODATA_OK;

    // as Rockbox's ata_sleepnow(): STANDBY IMMEDIATE writes back the cache
    // too, so it gets the time a flush gets
    t0 = GetTickCount();
    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);
    rc = WaitReadyForCommand(FLUSH_WAIT_MS);
    if (rc == IPODATA_OK && (!DiskWrite(ATA_PIO_DVR, 0) || !DiskWrite(ATA_PIO_CSD, CMD_STANDBY_IMMEDIATE)))
        rc = IPODATA_E_HANDSHAKE;
    if (rc == IPODATA_OK)
        rc = WaitNotBusy(FLUSH_WAIT_MS);
    if (rc == IPODATA_OK)
    {
        if (!DiskRead(ATA_PIO_DAD, &v))
            rc = IPODATA_E_HANDSHAKE;
        else if ((g_st.lastStatus = v & 0xFF) & ST_ERR)
            rc = NoteDeviceError();
    }
    if (rc != IPODATA_OK)
    {
        g_st.spinDownErrors++;
        Fail(IPODATA_STEP_STANDBY, rc);
        RETAILMSG(1, (L"ipodata: STANDBY IMMEDIATE FAILED (code %u status %02x error %02x)\r\n",
                      rc, g_st.lastStatus, g_st.lastError));
        return rc;
    }
    g_st.standby = 1;
    g_st.spinDowns++;
    RETAILMSG(1, (L"ipodata: disk spun down (standby) in %u ms\r\n", GetTickCount() - t0));
    return IPODATA_OK;
}

BOOL IpodAta_UseInterrupt(DWORD sysIntr)
{
    HANDLE hEvent;

    if (!g_st.mapped || sysIntr == 0 || g_hIntr != NULL)
        return FALSE;
    if (CLK_RD(CLK_PWRCON0) & PWRCON0_ATA)
        return FALSE;               // no clock: the controller's registers cannot be reached

    // Quiet the source before the line is enabled: Rockbox or the
    // bootloader may have left a status bit set or the mask on, and an
    // asserted line would storm the kernel before the first transfer.
    ATA_WR(ATA_IRQ_MASK, 0);
    ATA_WR(ATA_IRQ, ATA_IRQ_ALL);

    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hEvent == NULL)
    {
        g_st.intrError = GetLastError();
        return FALSE;
    }
    if (!InterruptInitialize(sysIntr, hEvent, NULL, 0))
    {
        g_st.intrError = GetLastError();
        CloseHandle(hEvent);
        RETAILMSG(1, (L"ipodata: InterruptInitialize(%u) failed, error %u: DMA completion stays polled\r\n",
                      sysIntr, g_st.intrError));
        return FALSE;
    }
    g_st.sysIntr = sysIntr;
    g_hIntr = hEvent;
    RETAILMSG(1, (L"ipodata: DMA completion by interrupt, SysIntr %u (ATA_IRQ was %x, mask %x at boot)\r\n",
                  sysIntr, g_st.bootIrq, g_st.bootIrqMask));
    return TRUE;
}

void IpodAta_GetStatus(IPODATA_STATUS *pOut)
{
    if (g_st.mapped && !(CLK_RD(CLK_PWRCON0) & PWRCON0_ATA))
    {
        g_st.control = ATA_RD(ATA_CONTROL);
        g_st.cfg = ATA_RD(ATA_CFG);
        g_st.pioTime = ATA_RD(ATA_PIO_TIME);
        g_st.irq = ATA_RD(ATA_IRQ);
        g_st.irqMask = ATA_RD(ATA_IRQ_MASK);
    }
    *pOut = g_st;
}
