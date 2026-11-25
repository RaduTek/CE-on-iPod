//
// i2chw.c - the S5L8702's I2C controller 0 under Windows CE, polled
//
// The controller as Rockbox drives it (Rockbox
// firmware/target/arm/s5l8702/i2c-s5l8702.c), moved here from the battery
// driver's ipodi2c.c (where reading the power chip was verified on hardware)
// and given a write path:
//   - the controller's clock is switched on for a transfer and off again
//     after it;
//   - a read is two transfers: the register number, a stop, then a new start
//     to read (no repeated start, as Rockbox does it);
//   - a write is one transfer: the register number, then the data bytes.
// Unlike Rockbox, every wait is bounded, and after a failure the bus is
// released and the clock switched off all the same.
//
// The controller's speed setting (IICCON) is what Rockbox writes before each
// transfer: no interrupt, acknowledge on, divider bits 0. The resulting bus
// clock has not been measured.
//
// Not reentrant: ipodi2c.c serialises.
//
#include <windows.h>
#include <ceddk.h>

#include <ipodi2c.h>
#include "i2chw.h"

// physical addresses (Rockbox firmware/export/s5l87xx.h, S5L8702)
#define PA_CLK              0x3C500000
#define PA_GPIO             0x3CF00000
#define PA_I2C0             0x3C600000

#define CLK_PWRCON1         0x4C        // bit 4 = I2C 0 clock gate (1 = off)
#define PWRCON1_I2C0        (1 << 4)
#define GPIO_PCON3          0x60        // bits 4..11: the two bus pins
#define PCON3_I2C_MASK      0x00000FF0
#define PCON3_I2C_VALUE     0x00000220

#define IICCON              0x00
#define IICSTAT             0x04
#define IICDS               0x0C
#define IICUNK10            0x10        // not 0 while the controller is busy with a register access
#define IICSTA2             0x20        // bits 8 and 13: a byte (or start/stop) has finished

#define IICCON_ACK          (1 << 7)
#define IICSTAT_BUSY        (1 << 5)
#define IICSTAT_NAK         (1 << 0)
#define IICSTA2_DONE        ((1 << 8) | (1 << 13))

#define READY_LOOPS         100000      // per register access
#define IO_LOOPS            200000      // per byte on the bus
#define SHORT_DELAY_LOOPS   200         // Rockbox waits 5 microseconds after loading a byte

static volatile UINT32 *g_pI2c = NULL, *g_pClk = NULL, *g_pGpio = NULL;
static volatile DWORD g_spin;

#define I2C_RD(off)         (g_pI2c[(off) >> 2])
#define I2C_WR(off, val)    (g_pI2c[(off) >> 2] = (val))
#define CLK_RD(off)         (g_pClk[(off) >> 2])
#define CLK_WR(off, val)    (g_pClk[(off) >> 2] = (val))
#define GPIO_RD(off)        (g_pGpio[(off) >> 2])

static void ShortDelay(void)
{
    int i;

    for (i = 0; i < SHORT_DELAY_LOOPS; i++)
        g_spin++;
}

// The controller is ready for the next register access.
static BOOL WaitRdy(void)
{
    int i;

    for (i = 0; i < READY_LOOPS; i++)
    {
        if (I2C_RD(IICUNK10) == 0)
            return TRUE;
    }
    return FALSE;
}

// The byte (or start, or stop) on the bus has finished. Returns an error code.
static DWORD WaitIo(void)
{
    int i;

    for (i = 0; i < IO_LOOPS; i++)
    {
        if ((I2C_RD(IICSTAT) & IICSTAT_BUSY) == 0 || (I2C_RD(IICSTA2) & IICSTA2_DONE) != 0)
        {
            I2C_WR(IICSTA2, I2C_RD(IICSTA2) | IICSTA2_DONE);
            return IPODI2C_OK;
        }
        if (!WaitRdy())
            return IPODI2C_E_NOT_READY;
    }
    return IPODI2C_E_NO_END;
}

// Start condition and the chip's address, for sending (read = 0) or reading (1).
static DWORD Start(BYTE chip, DWORD read)
{
    DWORD mode = read ? 0x80 : 0xC0, rc;

    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICCON, IICCON_ACK);                 // no interrupt, acknowledge on
    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICSTAT, mode);
    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICDS, ((chip << 1) & 0xFE) | read);
    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICSTAT, mode | 0x30);
    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    rc = WaitIo();
    if (rc != IPODI2C_OK)
        return rc;
    if (I2C_RD(IICSTAT) & IICSTAT_NAK)
        return IPODI2C_E_NO_ANSWER;
    return IPODI2C_OK;
}

// Stop condition. Returns an error code; always tries all of it.
static DWORD Stop(void)
{
    DWORD rc = IPODI2C_OK;

    if (!WaitRdy())
        rc = IPODI2C_E_NOT_READY;
    I2C_WR(IICSTAT, I2C_RD(IICSTAT) & ~0x20);
    if (!WaitRdy())
        rc = IPODI2C_E_NOT_READY;
    I2C_WR(IICCON, 0x10);
    if (rc == IPODI2C_OK)
        rc = WaitIo();
    return rc;
}

// One byte after the address: a register number or a data byte.
static DWORD SendByte(BYTE value)
{
    DWORD rc;

    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICDS, value);
    ShortDelay();
    if (!WaitRdy())
        return IPODI2C_E_NOT_READY;
    I2C_WR(IICCON, I2C_RD(IICCON));             // lets the byte go
    rc = WaitIo();
    if (rc != IPODI2C_OK)
        return rc;
    if (I2C_RD(IICSTAT) & IICSTAT_NAK)
        return IPODI2C_E_NO_ANSWER;
    return IPODI2C_OK;
}

static void ClockOn(void)
{
    CLK_WR(CLK_PWRCON1, CLK_RD(CLK_PWRCON1) & ~PWRCON1_I2C0);
}

static void BusOff(void)
{
    WaitRdy();
    I2C_WR(IICSTAT, 0);                         // serial output off
    WaitRdy();
    CLK_WR(CLK_PWRCON1, CLK_RD(CLK_PWRCON1) | PWRCON1_I2C0);
}

// The account of a failure, then stop (unless the stop itself failed), bus
// off. Reads the controller's registers first: only valid while its clock is on.
static DWORD Fail(I2CHW_FAIL *pFail, DWORD step, DWORD code, BOOL sendStop)
{
    pFail->step = step;
    pFail->code = code;
    pFail->stat = I2C_RD(IICSTAT);
    pFail->sta2 = I2C_RD(IICSTA2);
    if (sendStop)
        Stop();
    BusOff();
    return code;
}

// The checks before the bus is touched.
static DWORD Check(I2CHW_FAIL *pFail)
{
    memset(pFail, 0, sizeof(*pFail));
    if (g_pI2c == NULL || g_pClk == NULL || g_pGpio == NULL)
    {
        pFail->step = IPODI2C_STEP_CHECK;
        pFail->code = IPODI2C_E_NOT_MAPPED;
        return pFail->code;
    }
    if ((GPIO_RD(GPIO_PCON3) & PCON3_I2C_MASK) != PCON3_I2C_VALUE)
    {
        pFail->step = IPODI2C_STEP_CHECK;
        pFail->code = IPODI2C_E_PINS;
        return pFail->code;
    }
    return IPODI2C_OK;
}

//------------------------------------------------------------------------------
static volatile UINT32 *MapReg(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return (volatile UINT32 *)MmMapIoSpace(a, 0x1000, FALSE);
}

BOOL I2cHw_Map(DWORD *pBootPcon3, DWORD *pBootPwrcon1)
{
    g_pClk = MapReg(PA_CLK);
    g_pGpio = MapReg(PA_GPIO);
    g_pI2c = MapReg(PA_I2C0);
    if (g_pClk == NULL || g_pGpio == NULL || g_pI2c == NULL)
        return FALSE;
    *pBootPcon3 = GPIO_RD(GPIO_PCON3);
    *pBootPwrcon1 = CLK_RD(CLK_PWRCON1);
    return TRUE;
}

DWORD I2cHw_Read(BYTE chip, BYTE reg, BYTE *pOut, DWORD count, I2CHW_FAIL *pFail)
{
    DWORD rc, i;

    rc = Check(pFail);
    if (rc != IPODI2C_OK)
        return rc;
    ClockOn();

    // first transfer: which register
    rc = Start(chip, 0);
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_ADDRESS, rc, TRUE);
    rc = SendByte(reg);
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_REGISTER, rc, TRUE);
    rc = Stop();
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_STOP1, rc, FALSE);

    // second transfer: the data. Every byte but the last is acknowledged.
    rc = Start(chip, 1);
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_READ, rc, TRUE);
    for (i = 0; i < count; i++)
    {
        if (!WaitRdy())
            return Fail(pFail, IPODI2C_STEP_DATA, IPODI2C_E_NOT_READY, TRUE);
        if (i == count - 1)
            I2C_WR(IICCON, I2C_RD(IICCON) & ~IICCON_ACK);
        else
            I2C_WR(IICCON, I2C_RD(IICCON));
        rc = WaitIo();
        if (rc != IPODI2C_OK)
            return Fail(pFail, IPODI2C_STEP_DATA, rc, TRUE);
        pOut[i] = (BYTE)(I2C_RD(IICDS) & 0xFF);
    }
    rc = Stop();
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_STOP2, rc, FALSE);

    BusOff();
    return IPODI2C_OK;
}

DWORD I2cHw_Write(BYTE chip, BYTE reg, const BYTE *pData, DWORD count, I2CHW_FAIL *pFail)
{
    DWORD rc, i;

    rc = Check(pFail);
    if (rc != IPODI2C_OK)
        return rc;
    ClockOn();

    rc = Start(chip, 0);
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_ADDRESS, rc, TRUE);
    rc = SendByte(reg);
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_REGISTER, rc, TRUE);
    for (i = 0; i < count; i++)
    {
        rc = SendByte(pData[i]);
        if (rc != IPODI2C_OK)
            return Fail(pFail, IPODI2C_STEP_WDATA, rc, TRUE);
    }
    rc = Stop();
    if (rc != IPODI2C_OK)
        return Fail(pFail, IPODI2C_STEP_WSTOP, rc, FALSE);

    BusOff();
    return IPODI2C_OK;
}
