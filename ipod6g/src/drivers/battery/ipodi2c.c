//
// ipodi2c.c - reading a chip on the iPod's I2C bus 0 under Windows CE
//
// The bus controller of the S5L8702 as Rockbox drives it (Rockbox
// firmware/target/arm/s5l8702/i2c-s5l8702.c), polled, for the power chip
// (PCF5063x family, address 0xE6): the clock, the charger status and the
// battery measurement live there, and so do the iPod's supply voltages.
//
// WHAT THIS FILE CAN PUT ON THE BUS, and nothing else:
//   - to read registers: the chip's address and the number of the first
//     register (and the address again to turn the bus around);
//   - to start a battery voltage measurement: the two fixed byte strings
//     g_measure1 and g_measure2 below, to the power chip. They are what Rockbox
//     sends for the same purpose (pmu-6g.c pmu_read_adc with the
//     ADC_BATTERY channel of adc-6g.c).
// No function takes bytes to send from its caller.
//
// As Rockbox does: the controller's clock is switched on for a transfer and off
// again after it; a read is two transfers (register number, then the data).
// Unlike Rockbox, every wait is bounded, and after a failure the bus is released
// and the clock switched off all the same.
//
// Not reentrant: the caller serialises.
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>

#include <ipodi2c.h>

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

// The PC test redirects these into its model of the controller and the chip.
#ifndef I2C_RD
static volatile UINT32 *g_pI2c = NULL, *g_pClk = NULL, *g_pGpio = NULL;
#define I2C_RD(off)         (g_pI2c[(off) >> 2])
#define I2C_WR(off, val)    (g_pI2c[(off) >> 2] = (val))
#define CLK_RD(off)         (g_pClk[(off) >> 2])
#define CLK_WR(off, val)    (g_pClk[(off) >> 2] = (val))
#define GPIO_RD(off)        (g_pGpio[(off) >> 2])
#define IPODI2C_MAP_REGISTERS
#endif

// The battery measurement of the PCF5063x, as Rockbox starts it:
//   register 0x52 (ADCC3) = 0x00   no divider, no accessory switch
//   register 0x53 (ADCC2) = 0x00   no ratio settings
//   register 0x54 (ADCC1) = 0x15   battery minus 2.25 V, average of 4, 10 bits, start
// The second string writes 0x53 and 0x54 in one transfer (the register
// number counts up by itself).
static const BYTE g_measure1[] = { 0x52, 0x00 };
static const BYTE g_measure2[] = { 0x53, 0x00, 0x15 };

static IPODI2C_STATE g_bus;
static volatile DWORD g_spin;

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

static void Note(void)
{
    g_bus.lastStat = I2C_RD(IICSTAT);
    g_bus.lastSta2 = I2C_RD(IICSTA2);
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
    I2C_WR(IICDS, (chip & 0xFE) | read);
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

// One byte after the address: a register number, or a byte of g_measure1 / g_measure2.
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

static void BusOff(void)
{
    WaitRdy();
    I2C_WR(IICSTAT, 0);                         // serial output off
    WaitRdy();
    CLK_WR(CLK_PWRCON1, CLK_RD(CLK_PWRCON1) | PWRCON1_I2C0);
}

// The account of a failure. The controller's own registers are added by
// Note(), which may only be called while its clock is on.
static DWORD Fail(DWORD step, DWORD code)
{
    g_bus.lastStep = step;
    g_bus.lastCode = code;
    g_bus.errors++;
    return code;
}

//------------------------------------------------------------------------------
#ifdef IPODI2C_MAP_REGISTERS
static void *MapReg(DWORD pa)
{
    PHYSICAL_ADDRESS a;

    a.QuadPart = pa;
    return MmMapIoSpace(a, 0x1000, FALSE);
}
#endif

BOOL IpodI2c_Init(void)
{
    memset(&g_bus, 0, sizeof(g_bus));
#ifdef IPODI2C_MAP_REGISTERS
    g_pClk = (volatile UINT32 *)MapReg(PA_CLK);
    g_pGpio = (volatile UINT32 *)MapReg(PA_GPIO);
    g_pI2c = (volatile UINT32 *)MapReg(PA_I2C0);
    if (g_pClk == NULL || g_pGpio == NULL || g_pI2c == NULL)
    {
        RETAILMSG(1, (L"ipodi2c: cannot map the registers\r\n"));
        return FALSE;
    }
#endif
    g_bus.mapped = 1;
    g_bus.bootPcon3 = GPIO_RD(GPIO_PCON3);
    g_bus.bootPwrcon1 = CLK_RD(CLK_PWRCON1);
    RETAILMSG(1, (L"ipodi2c: bus pins %08x, clock gates %08x\r\n", g_bus.bootPcon3, g_bus.bootPwrcon1));
    return TRUE;
}

DWORD IpodI2c_Read(BYTE chip, BYTE firstReg, BYTE *pOut, DWORD count)
{
    DWORD rc, step, i;

    g_bus.lastStep = 0;
    g_bus.lastCode = IPODI2C_OK;
    g_bus.lastStat = 0;
    g_bus.lastSta2 = 0;

    if (!g_bus.mapped)
        return Fail(IPODI2C_STEP_CHECK, IPODI2C_E_NOT_MAPPED);
    if (pOut == NULL || count == 0 || count > IPODI2C_MAX_READ)
        return Fail(IPODI2C_STEP_CHECK, IPODI2C_E_PARAM);
    if ((GPIO_RD(GPIO_PCON3) & PCON3_I2C_MASK) != PCON3_I2C_VALUE)
        return Fail(IPODI2C_STEP_CHECK, IPODI2C_E_PINS);

    CLK_WR(CLK_PWRCON1, CLK_RD(CLK_PWRCON1) & ~PWRCON1_I2C0);      // the controller's clock on

    // first transfer: which register
    step = IPODI2C_STEP_ADDRESS;
    rc = Start(chip, 0);
    if (rc == IPODI2C_OK)
    {
        step = IPODI2C_STEP_REGISTER;
        rc = SendByte(firstReg);
    }
    if (rc != IPODI2C_OK)
    {
        Note();
        g_bus.lastStep = step;          // before the stop changes the picture
        g_bus.lastCode = rc;
        g_bus.errors++;
        Stop();
        BusOff();
        return rc;
    }
    rc = Stop();
    if (rc != IPODI2C_OK)
    {
        Note();
        Fail(IPODI2C_STEP_STOP1, rc);
        BusOff();
        return rc;
    }

    // second transfer: the data. Every byte but the last is acknowledged.
    step = IPODI2C_STEP_READ;
    rc = Start(chip, 1);
    for (i = 0; rc == IPODI2C_OK && i < count; i++)
    {
        step = IPODI2C_STEP_DATA;
        if (!WaitRdy())
        {
            rc = IPODI2C_E_NOT_READY;
            break;
        }
        if (i == count - 1)
            I2C_WR(IICCON, I2C_RD(IICCON) & ~IICCON_ACK);
        else
            I2C_WR(IICCON, I2C_RD(IICCON));
        rc = WaitIo();
        if (rc == IPODI2C_OK)
            pOut[i] = (BYTE)(I2C_RD(IICDS) & 0xFF);
    }
    if (rc != IPODI2C_OK)
    {
        Note();
        g_bus.lastStep = step;
        g_bus.lastCode = rc;
        g_bus.errors++;
        Stop();
        BusOff();
        return rc;
    }
    rc = Stop();
    if (rc != IPODI2C_OK)
    {
        Note();
        Fail(IPODI2C_STEP_STOP2, rc);
        BusOff();
        return rc;
    }

    BusOff();
    g_bus.reads++;
    return IPODI2C_OK;
}

// One of the two fixed strings to the power chip, as one transfer.
static DWORD SendFixed(const BYTE *pBytes, DWORD count)
{
    DWORD rc, i;

    rc = Start(IPODI2C_PMU, 0);
    for (i = 0; rc == IPODI2C_OK && i < count; i++)
        rc = SendByte(pBytes[i]);
    if (rc != IPODI2C_OK)
    {
        Note();
        Stop();
        return rc;
    }
    return Stop();
}

DWORD IpodI2c_StartBatteryMeasurement(void)
{
    DWORD rc;

    g_bus.lastStep = 0;
    g_bus.lastCode = IPODI2C_OK;

    if (!g_bus.mapped)
        return Fail(IPODI2C_STEP_CHECK, IPODI2C_E_NOT_MAPPED);
    if ((GPIO_RD(GPIO_PCON3) & PCON3_I2C_MASK) != PCON3_I2C_VALUE)
        return Fail(IPODI2C_STEP_CHECK, IPODI2C_E_PINS);

    CLK_WR(CLK_PWRCON1, CLK_RD(CLK_PWRCON1) & ~PWRCON1_I2C0);      // the controller's clock on

    rc = SendFixed(g_measure1, sizeof(g_measure1));
    if (rc != IPODI2C_OK)
    {
        Note();
        g_bus.lastStep = IPODI2C_STEP_MEASURE1;
        g_bus.lastCode = rc;
        g_bus.errors++;
        BusOff();
        return rc;
    }
    rc = SendFixed(g_measure2, sizeof(g_measure2));
    if (rc != IPODI2C_OK)
    {
        Note();
        g_bus.lastStep = IPODI2C_STEP_MEASURE2;
        g_bus.lastCode = rc;
        g_bus.errors++;
        BusOff();
        return rc;
    }

    BusOff();
    g_bus.measurements++;
    return IPODI2C_OK;
}

void IpodI2c_GetState(IPODI2C_STATE *pOut)
{
    *pOut = g_bus;
}
