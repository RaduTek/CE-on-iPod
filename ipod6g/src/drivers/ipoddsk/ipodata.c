//
// ipodata.c - reading the iPod's disk under Windows CE (ATA, PIO, read only)
//
// The disk controller of the S5L8702 as Rockbox drives it (Rockbox
// firmware/target/arm/s5l8702/ipod6g/storage_ata-6g.c, the part that is not
// CE-ATA). Rockbox has just loaded Windows CE from this disk, so the disk is
// powered, its pins are set up and the controller is enabled; none of that
// is redone here. No reset, no power switching, no settings sent to the disk.
//
// READ ONLY BY CONSTRUCTION: the only commands this file can send to the
// disk are IDENTIFY DEVICE (0xEC), READ SECTORS (0x20) and READ SECTORS EXT
// (0x24). There is no write function and no way to pass a command in.
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
#define ATA_PIO_TIME        0x2C
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

#define ATA_COMMAND_STOP    (1 << 1)
#define ATA_CFG_PIO         (1 << 6)    // the configuration Rockbox reads the identify data in

#define CMD_READ_SECTORS        0x20
#define CMD_READ_SECTORS_EXT    0x24
#define CMD_IDENTIFY            0xEC

#define ST_ERR              0x01
#define ST_DRQ              0x08
#define ST_DF               0x20
#define ST_DRDY             0x40
#define ST_BSY              0x80

#define READY_LOOPS         200000      // per register access
#define SPIN_LOOPS          50          // short waits are spun, longer ones sleep
#define IDENTIFY_WAIT_MS    5000
#define READ_WAIT_MS        2000

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
    }
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

DWORD IpodAta_Attach(void)
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

    g_st.attached = 1;
    return IPODATA_OK;
}

DWORD IpodAta_Read(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE *pOut)
{
    DWORD rc, i, t0, endLow, endHigh;
    BOOL ok;

    g_st.lastStep = 0;
    g_st.lastCode = IPODATA_OK;
    g_st.lastStatus = 0;
    g_st.lastError = 0;

    if (!g_st.attached)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_NOT_ATTACHED);
    if (g_st.sectorBytes != IPODATA_SECTOR_BYTES)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_SECTOR_SIZE);
    if (pOut == NULL || count == 0)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_PARAM);
    if (count > IPODATA_MAX_SECTORS)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    // lba + count must not pass the end of the disk (64-bit, done in halves)
    endLow = lbaLow + count;
    endHigh = lbaHigh + (endLow < lbaLow ? 1 : 0);
    if (endHigh < lbaHigh || endHigh > g_st.sectorsHigh
        || (endHigh == g_st.sectorsHigh && endLow > g_st.sectorsLow))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    if (!g_st.lba48 && (lbaHigh != 0 || lbaLow > 0x0FFFFFFF))
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);
    if (g_st.lba48 && lbaHigh > 0xFFFF)
        return Fail(IPODATA_STEP_CHECK, IPODATA_E_RANGE);

    t0 = GetTickCount();
    g_st.reads++;
    ATA_WR(ATA_COMMAND, ATA_COMMAND_STOP);          // as Rockbox does before every transfer

    rc = WaitReadyForCommand(READ_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_RD_READY, rc);

    ok = DiskWrite(ATA_PIO_DVR, 0);
    if (g_st.lba48)
    {
        // the "previous" half of each register first, then the current one
        ok = ok && DiskWrite(ATA_PIO_SCR, 0);
        ok = ok && DiskWrite(ATA_PIO_SCR, count & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaHigh >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, lbaHigh & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, (lbaLow >> 24) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaLow >> 16) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, (lbaLow >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, lbaLow & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_DVR, 1 << 6);
        ok = ok && DiskWrite(ATA_PIO_CSD, CMD_READ_SECTORS_EXT);
    }
    else
    {
        ok = ok && DiskWrite(ATA_PIO_SCR, count & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LHR, (lbaLow >> 16) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LMR, (lbaLow >> 8) & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_LLR, lbaLow & 0xFF);
        ok = ok && DiskWrite(ATA_PIO_DVR, (1 << 6) | ((lbaLow >> 24) & 0x0F));
        ok = ok && DiskWrite(ATA_PIO_CSD, CMD_READ_SECTORS);
    }
    if (!ok)
        return Fail(IPODATA_STEP_RD_CMD, IPODATA_E_HANDSHAKE);

    for (i = 0; i < count; i++)
    {
        rc = WaitForData(READ_WAIT_MS);
        if (rc != IPODATA_OK)
            return Fail(IPODATA_STEP_RD_DATA, rc);
        if (!TakeWords(pOut + i * IPODATA_SECTOR_BYTES, IPODATA_SECTOR_BYTES / 2))
            return Fail(IPODATA_STEP_RD_DATA, IPODATA_E_HANDSHAKE);
        g_st.sectorsRead++;
    }
    rc = WaitForEnd(READ_WAIT_MS);
    if (rc != IPODATA_OK)
        return Fail(IPODATA_STEP_RD_END, rc);

    g_st.lastReadMs = GetTickCount() - t0;
    g_st.lastReadSectors = count;
    return IPODATA_OK;
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
