//
// touchprobe.c - touch controller bring-up probe (steps 1 and 2)
//
// Touch bring-up (docs\ipodtouch2g.md, "Touch"), once at the end of OEMInit.
//
// Step 1, read only: how iBoot left
//   - the GPIO ports (touch data ready 0x0a05, reset 0x0a06 and chip select
//     0x0a07 are port 10, bits 5-7) and the GPIO interrupt groups (data
//     ready is GPIO interrupt 0x6D: group 3, bit 13)
//   - the SPI4 controller (the touch controller's bus), if its clock is on
//   - all 256 registers of the power chip (Dialog D1759 on I2C bus 0,
//     address 0x73), if the I2C clock is on. LDO 3, the touch supply, is
//     register 0x10 bit 5 (n72-linux): on at 1.8 V on 2026-11-28.
// Nothing is written except the I2C controller's own registers during the
// reads. Reading the power chip might clear its interrupt/event flags if
// some registers are clear-on-read; nothing in the BSP uses them yet.
//
// Step 2, only if LDO 3 is on: reset pulses with the data-ready line
// watched (ProbeTouchReset). Writes the reset pin and, in run B, SPI4's
// setup register; both are put back as found.
//
// The I2C steps are those of the ipod6g BSP (src\drivers\ipodi2c\i2chw.c,
// verified on the S5L8702, which has the same controller), polled and with
// every wait bounded. Output goes through Debug(), so it shows on the UART
// in builds without KITL.
//
#include <windows.h>
#include <oemglobal.h>

#include <bsp.h>

#ifdef BSP_TOUCHPROBE

void Debug(LPCTSTR szFormat, ...);

#define READY_LOOPS         100000      // per register access
#define IO_LOOPS            200000      // per byte on the bus
#define SHORT_DELAY_LOOPS   2000        // a few microseconds after loading a byte

#define I2C_RD(off)         S5L_REG(S5L_I2C0_PA + (off))
#define I2C_WR(off, val)    (S5L_REG(S5L_I2C0_PA + (off)) = (val))

#define IICCON_ACK          (1 << 7)
#define IICSTAT_BUSY        (1 << 5)
#define IICSTAT_NAK         (1 << 0)
#define IICSTA2_DONE        ((1 << 8) | (1 << 13))

// error codes of the probe's I2C reads
#define PRB_OK              0
#define PRB_E_NOT_READY     1           // the controller stayed busy
#define PRB_E_NO_END        2           // a byte never finished
#define PRB_E_NO_ANSWER     3           // no acknowledge from the chip

static volatile DWORD g_probeSpin;

static void ShortDelay(void)
{
    int i;

    for (i = 0; i < SHORT_DELAY_LOOPS; i++)
        g_probeSpin++;
}

static BOOL WaitRdy(void)
{
    int i;

    for (i = 0; i < READY_LOOPS; i++)
    {
        if (I2C_RD(I2C_IICUNK10) == 0)
            return TRUE;
    }
    return FALSE;
}

static DWORD WaitIo(void)
{
    int i;

    for (i = 0; i < IO_LOOPS; i++)
    {
        if ((I2C_RD(I2C_IICSTAT) & IICSTAT_BUSY) == 0 || (I2C_RD(I2C_IICSTA2) & IICSTA2_DONE) != 0)
        {
            I2C_WR(I2C_IICSTA2, I2C_RD(I2C_IICSTA2) | IICSTA2_DONE);
            return PRB_OK;
        }
        if (!WaitRdy())
            return PRB_E_NOT_READY;
    }
    return PRB_E_NO_END;
}

static DWORD Start(BYTE chip, DWORD read)
{
    DWORD mode = read ? 0x80 : 0xC0, rc;

    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICCON, IICCON_ACK);             // no interrupt, acknowledge on
    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICSTAT, mode);
    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICDS, ((chip << 1) & 0xFE) | read);
    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICSTAT, mode | 0x30);
    if (!WaitRdy())
        return PRB_E_NOT_READY;
    rc = WaitIo();
    if (rc != PRB_OK)
        return rc;
    if (I2C_RD(I2C_IICSTAT) & IICSTAT_NAK)
        return PRB_E_NO_ANSWER;
    return PRB_OK;
}

static DWORD Stop(void)
{
    DWORD rc = PRB_OK;

    if (!WaitRdy())
        rc = PRB_E_NOT_READY;
    I2C_WR(I2C_IICSTAT, I2C_RD(I2C_IICSTAT) & ~0x20);
    if (!WaitRdy())
        rc = PRB_E_NOT_READY;
    I2C_WR(I2C_IICCON, 0x10);
    if (rc == PRB_OK)
        rc = WaitIo();
    return rc;
}

static DWORD SendByte(BYTE value)
{
    DWORD rc;

    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICDS, value);
    ShortDelay();
    if (!WaitRdy())
        return PRB_E_NOT_READY;
    I2C_WR(I2C_IICCON, I2C_RD(I2C_IICCON));     // lets the byte go
    rc = WaitIo();
    if (rc != PRB_OK)
        return rc;
    if (I2C_RD(I2C_IICSTAT) & IICSTAT_NAK)
        return PRB_E_NO_ANSWER;
    return PRB_OK;
}

// Read 'count' registers from 'reg' on. *pStep says where it failed
// (1 address, 2 register, 3 first stop, 4 read address, 5 data, 6 stop).
static DWORD I2cRead(BYTE chip, BYTE reg, BYTE *pOut, DWORD count, DWORD *pStep)
{
    DWORD rc, i;

    *pStep = 1;
    rc = Start(chip, 0);
    if (rc != PRB_OK) goto fail;
    *pStep = 2;
    rc = SendByte(reg);
    if (rc != PRB_OK) goto fail;
    *pStep = 3;
    rc = Stop();
    if (rc != PRB_OK) goto failNoStop;

    *pStep = 4;
    rc = Start(chip, 1);
    if (rc != PRB_OK) goto fail;
    *pStep = 5;
    for (i = 0; i < count; i++)
    {
        if (!WaitRdy())
        {
            rc = PRB_E_NOT_READY;
            goto fail;
        }
        if (i == count - 1)
            I2C_WR(I2C_IICCON, I2C_RD(I2C_IICCON) & ~IICCON_ACK);
        else
            I2C_WR(I2C_IICCON, I2C_RD(I2C_IICCON));
        rc = WaitIo();
        if (rc != PRB_OK) goto fail;
        pOut[i] = (BYTE)(I2C_RD(I2C_IICDS) & 0xFF);
    }
    *pStep = 6;
    rc = Stop();
    if (rc != PRB_OK) goto failNoStop;
    return PRB_OK;

fail:
    Stop();
failNoStop:
    return rc;
}

// ---------------------------------------------------------------------------
// GPIO ports and interrupt groups
//
static void ProbeGpio(void)
{
    UINT32 p, g, con, dat;

    Debug(L"touchprobe: GPIO ports (CON DAT PUD1 PUD2):\r\n");
    for (p = 0; p < S5L_GPIO_NUMPORTS; p++)
    {
        Debug(L"touchprobe:   port %2d  %08x %08x %08x %08x\r\n", p,
              S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_CON), S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_DAT),
              S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_PUD1), S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_PUD2));
    }

    p = GPIO_PIN_PORT(TOUCH_GPIO_RESET);
    con = S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_CON);
    dat = S5L_REG(S5L_GPIO_PORT_PA(p) + GPIO_DAT);
    Debug(L"touchprobe: reset pin 0x%03x: function %x, level %d; chip select pin 0x%03x: function %x, level %d\r\n",
          TOUCH_GPIO_RESET, (con >> (4 * GPIO_PIN_BIT(TOUCH_GPIO_RESET))) & 0xF,
          (dat >> GPIO_PIN_BIT(TOUCH_GPIO_RESET)) & 1,
          TOUCH_GPIO_CS, (con >> (4 * GPIO_PIN_BIT(TOUCH_GPIO_CS))) & 0xF,
          (dat >> GPIO_PIN_BIT(TOUCH_GPIO_CS)) & 1);

    Debug(L"touchprobe: GPIO interrupt groups (LEVEL STAT EN TYPE):\r\n");
    for (g = 0; g < GPIOIC_NUMGROUPS; g++)
    {
        Debug(L"touchprobe:   group %d  %08x %08x %08x %08x\r\n", g,
              S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTLEVEL + 4 * g), S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTSTAT + 4 * g),
              S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTEN + 4 * g), S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTTYPE + 4 * g));
    }
    g = TOUCH_GPIOINT_ATN >> 5;
    Debug(L"touchprobe: ATN (GPIO interrupt 0x%02x = group %d bit %d): status %d, enabled %d; GPIOIC 6C %08x 70 %08x 7C %08x\r\n",
          TOUCH_GPIOINT_ATN, g, TOUCH_GPIOINT_ATN & 0x1F,
          (S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTSTAT + 4 * g) >> (TOUCH_GPIOINT_ATN & 0x1F)) & 1,
          (S5L_REG(S5L_GPIOIC_PA + GPIOIC_INTEN + 4 * g) >> (TOUCH_GPIOINT_ATN & 0x1F)) & 1,
          S5L_REG(S5L_GPIOIC_PA + 0x6C), S5L_REG(S5L_GPIOIC_PA + 0x70), S5L_REG(S5L_GPIOIC_PA + 0x7C));
}

// ---------------------------------------------------------------------------
// SPI4 (skips the receive register, which pops the FIFO)
//
static void ProbeSpi4(void)
{
    UINT32 g2 = S5L_REG(S5L_CLKGATES2_PA), g4 = S5L_REG(S5L_CLKGATES4_PA);

    Debug(L"touchprobe: SPI4 clock gates: gates2 bit 4 = %d, gates4 bit 20 = %d (1 = off)\r\n",
          (g2 & SPI4_GATES2_BIT) ? 1 : 0, (g4 & SPI4_GATES4_BIT) ? 1 : 0);
    if ((g2 & SPI4_GATES2_BIT) || (g4 & SPI4_GATES4_BIT))
    {
        Debug(L"touchprobe: SPI4 clock off, registers not read\r\n");
        return;
    }

    Debug(L"touchprobe: SPI4 control %08x setup %08x status %08x pin %08x clkdiv %08x cnt %08x idd %08x txlen %08x\r\n",
          S5L_REG(S5L_SPI4_PA + SPI_CONTROL), S5L_REG(S5L_SPI4_PA + SPI_SETUP),
          S5L_REG(S5L_SPI4_PA + SPI_STATUS), S5L_REG(S5L_SPI4_PA + SPI_PIN),
          S5L_REG(S5L_SPI4_PA + SPI_CLKDIVIDER), S5L_REG(S5L_SPI4_PA + SPI_CNT),
          S5L_REG(S5L_SPI4_PA + SPI_IDD), S5L_REG(S5L_SPI4_PA + SPI_TXBUFFERLEN));
}

// ---------------------------------------------------------------------------
// Power chip: all 256 registers in 16-byte reads
//
static void ProbePmu(void)
{
    UINT32 g1 = S5L_REG(S5L_CLKGATES1_PA), g4 = S5L_REG(S5L_CLKGATES4_PA);
    BYTE buf[16];
    DWORD reg, step, rc, i;
    WCHAR line[80];

    Debug(L"touchprobe: I2C0 clock gates: gates1 bit 4 = %d, gates4 bit 11 = %d (1 = off); IICCON %08x IICSTAT %08x\r\n",
          (g1 & I2C0_GATES1_BIT) ? 1 : 0, (g4 & I2C0_GATES4_BIT) ? 1 : 0,
          I2C_RD(I2C_IICCON), I2C_RD(I2C_IICSTAT));
    if ((g1 & I2C0_GATES1_BIT) || (g4 & I2C0_GATES4_BIT))
    {
        Debug(L"touchprobe: I2C0 clock off, power chip not read\r\n");
        return;
    }

    Debug(L"touchprobe: power chip (I2C0 address 0x%02x) registers:\r\n", S5L_PMU_I2C_ADDR);
    for (reg = 0; reg < 0x100; reg += 16)
    {
        rc = I2cRead(S5L_PMU_I2C_ADDR, (BYTE)reg, buf, 16, &step);
        if (rc != PRB_OK)
        {
            Debug(L"touchprobe:   %02x: read failed, error %d at step %d (IICSTAT %08x STA2 %08x)\r\n",
                  reg, rc, step, I2C_RD(I2C_IICSTAT), I2C_RD(I2C_IICSTA2));
            if (rc == PRB_E_NO_ANSWER && reg == 0)
                return;                         // nobody at that address: stop here
            continue;
        }
        for (i = 0; i < 16; i++)
        {
            static const WCHAR hex[] = L"0123456789abcdef";
            line[i * 3]     = hex[buf[i] >> 4];
            line[i * 3 + 1] = hex[buf[i] & 0xF];
            line[i * 3 + 2] = L' ';
        }
        line[47] = L'\0';
        Debug(L"touchprobe:   %02x: %s\r\n", reg, line);
    }

    // leave the controller idle, as before (serial output off)
    WaitRdy();
    I2C_WR(I2C_IICSTAT, 0);
}

// ---------------------------------------------------------------------------
// Step 2: reset pulse, watch the data-ready line
//
// Reset is active-high (GPIO 86), data ready active-low (GPIO 85), as the
// n72-linux device tree describes them. Linux's zephyr2.c: reset asserted
// 200 ms, SCLK idle-high (SPI mode 3) before reset is released, then 15 ms
// until the controller's bootloader is up and announces itself (the "HBPP
// ready banner"), which pulls data ready low. Without an SPI read the
// banner is never taken, so the line should stay low once it drops.
//
#define TICKS_PER_US        12          // 12 MHz tick counter (verified)

static void DelayUs(UINT32 us)
{
    UINT64 end = S5lReadTicks() + (UINT64)us * TICKS_PER_US;

    while (S5lReadTicks() < end)
        ;
}

// Drive a pin through the function-select register, as openiBoot does
static void PinOut(UINT32 pin, BOOL high)
{
    S5L_REG(S5L_GPIO_FSEL_PA) = GPIO_FSEL_VALUE(pin, high ? GPIO_FSEL_OUT_HIGH : GPIO_FSEL_OUT_LOW);
}

static UINT32 PinLevel(UINT32 pin)
{
    return (S5L_REG(S5L_GPIO_PORT_PA(GPIO_PIN_PORT(pin)) + GPIO_DAT) >> GPIO_PIN_BIT(pin)) & 1;
}

#ifdef TOUCHPROBE_STEP2
// Step 2 result (2026-11-28, both runs): data ready low in reset, high
// 10 us after release, low again from ~35 us on. Kept for reference;
// define TOUCHPROBE_STEP2 to run it again.
#define WATCH_MS            500
#define MAX_EVENTS          32

typedef struct {
    UINT32  us;                         // since reset was released
    UINT32  port10;                     // port 10 data (pins 85, 86, 87 = bits 5, 6, 7)
    UINT32  stat3;                      // GPIO interrupt group 3 status (bit 13 = 0x6d)
} TOUCH_EVENT;

static TOUCH_EVENT g_events[MAX_EVENTS];

static void ResetRun(const WCHAR *pszName, BOOL fMode3)
{
    UINT32 group = TOUCH_GPIOINT_ATN >> 5;
    UINT32 statReg = S5L_GPIOIC_PA + GPIOIC_INTSTAT + 4 * group;
    UINT32 setupOld = S5L_REG(S5L_SPI4_PA + SPI_SETUP);
    UINT32 p10, st, lastP10, lastSt, initP10, initSt, n = 0, lost = 0, i;
    UINT64 t0, now, end;

    Debug(L"touchprobe: run %s: reset 0x%03x high (held in reset) for 200 ms\r\n",
          pszName, TOUCH_GPIO_RESET);
    PinOut(TOUCH_GPIO_RESET, TRUE);
    DelayUs(1000);
    Debug(L"touchprobe:   reset pin reads %d, data ready %d, CS %d\r\n",
          PinLevel(TOUCH_GPIO_RESET), PinLevel(TOUCH_GPIO_ATN), PinLevel(TOUCH_GPIO_CS));
    DelayUs(199000);

    if (fMode3)
    {
        // SCLK idle high before the controller leaves reset
        S5L_REG(S5L_SPI4_PA + SPI_SETUP) =
            setupOld | SPISETUP_LAST_CLOCK_EDGE_MISSING | SPISETUP_IS_ACTIVE_LOW;
        DelayUs(100);
        Debug(L"touchprobe:   SPI4 setup 0x%08x -> 0x%08x (mode 3)\r\n",
              setupOld, S5L_REG(S5L_SPI4_PA + SPI_SETUP));
    }

    // release reset and record every change of port 10 or the group 3 status
    initP10 = lastP10 = S5L_REG(S5L_GPIO_PORT_PA(10) + GPIO_DAT);
    initSt = lastSt = S5L_REG(statReg);
    PinOut(TOUCH_GPIO_RESET, FALSE);
    t0 = S5lReadTicks();
    end = t0 + (UINT64)WATCH_MS * 1000 * TICKS_PER_US;
    do
    {
        now = S5lReadTicks();
        p10 = S5L_REG(S5L_GPIO_PORT_PA(10) + GPIO_DAT);
        st = S5L_REG(statReg);
        if (p10 != lastP10 || st != lastSt)
        {
            if (n < MAX_EVENTS)
            {
                g_events[n].us = (UINT32)((now - t0) / TICKS_PER_US);
                g_events[n].port10 = p10;
                g_events[n].stat3 = st;
                n++;
            }
            else
            {
                lost++;
            }
            lastP10 = p10;
            lastSt = st;
        }
    } while (now < end);

    Debug(L"touchprobe:   before release: port 10 %08x (data ready %d, reset %d, CS %d), group 3 status %08x\r\n",
          initP10, (initP10 >> 5) & 1, (initP10 >> 6) & 1, (initP10 >> 7) & 1, initSt);
    for (i = 0; i < n; i++)
    {
        Debug(L"touchprobe:   +%6d us: port 10 %08x (data ready %d, reset %d, CS %d), group 3 status %08x (bit 13 = %d)\r\n",
              g_events[i].us, g_events[i].port10,
              (g_events[i].port10 >> 5) & 1, (g_events[i].port10 >> 6) & 1, (g_events[i].port10 >> 7) & 1,
              g_events[i].stat3, (g_events[i].stat3 >> 13) & 1);
    }
    Debug(L"touchprobe:   %d changes in %d ms (%d not recorded); at the end: data ready %d, group 3 status %08x\r\n",
          n + lost, WATCH_MS, lost, PinLevel(TOUCH_GPIO_ATN), S5L_REG(statReg));

    // back as found: held in reset, SPI4 setup unchanged
    PinOut(TOUCH_GPIO_RESET, TRUE);
    if (fMode3)
        S5L_REG(S5L_SPI4_PA + SPI_SETUP) = setupOld;
    DelayUs(1000);
}
#endif // TOUCHPROBE_STEP2

// ---------------------------------------------------------------------------
// Step 3: SPI link to the controller's bootloader (no firmware)
//
// SPI4 as n72-linux's spi-s5l8720.c drives it: fixed 24 MHz clock, master,
// mode 3, 8-bit words, polled FIFO (16 words each way); chip select is a
// GPIO, low during a transfer with 10 us before and after. Zephyr2 timing
// from zephyr2.c: 83 kHz ("normal"), 5 ms after a send, 10 ms after a
// transfer whose answer matters. Sequence of zephyr2.c z2_boot: reset on
// 200 ms, a 16-byte dummy transfer (SCLK idles high from then on), reset
// off, 15 ms, the dummy transfer again (takes the "HBPP ready banner"),
// then short acks (1a a1) as openiBoot reads them before a firmware upload
// (it repeats them until 0xffff). Everything received is logged.
//
#define SPI4_REG(off)           S5L_REG(S5L_SPI4_PA + (off))

#define SPICTL_ENABLE           (1 << 0)
#define SPICTL_RESET_TX         (1 << 2)
#define SPICTL_RESET_RX         (1 << 3)
#define SPISETUP_MODE3_8BIT     ((1 << 14) | (3 << 3) | (1 << 2) | (1 << 1))    // fixed clock, master, CPOL, CPHA
#define SPISETUP_OPTION5        (1 << 5)
#define SPISETUP_RX_ENABLE      (1 << 7)
#define SPISETUP_ENABLE2        (1 << 8)
#define SPISETUP_TX_ENABLE      (1 << 21)
#define SPISTATUS_CLEAR         0x0040000F
#define SPISTATUS_TXLVL(s)      (((s) >> 6) & 0x1F)
#define SPISTATUS_RXLVL(s)      (((s) >> 11) & 0x1F)
#define SPI_FIFO_DEPTH          16

#define Z2_DIV_NORMAL           290     // 24 MHz / 290 = 82.8 kHz (Linux: DIV_ROUND_UP(24 MHz, 83 kHz))
#define Z2_TX_DELAY_US          5000
#define Z2_RX_DELAY_US          10000
#define Z2_CS_DELAY_US          10
#define SPI_PROGRESS_US         20000   // no FIFO progress for this long = timeout

// One polled transfer of len bytes (len <= 64 here). FALSE on a timeout.
static BOOL SpiXfer(const BYTE *pTx, BYTE *pRx, UINT32 len, UINT32 delayAfterUs, UINT32 *pStatus)
{
    UINT32 setup = SPISETUP_MODE3_8BIT;
    UINT32 txSent, rxDone = 0, st = 0, avail, inFlight, space, push, i, b;
    UINT64 end;
    BOOL ok = TRUE;

    SPI4_REG(SPI_CLKDIVIDER) = Z2_DIV_NORMAL;
    SPI4_REG(SPI_IDD) = 0;

    PinOut(TOUCH_GPIO_CS, FALSE);
    DelayUs(Z2_CS_DELAY_US);

    SPI4_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    SPI4_REG(SPI_SETUP) = setup;
    SPI4_REG(SPI_CONTROL) = SPICTL_ENABLE | SPICTL_RESET_TX | SPICTL_RESET_RX;
    SPI4_REG(SPI_CNT) = len;
    SPI4_REG(SPI_TXBUFFERLEN) = len;
    SPI4_REG(SPI_SETUP) = setup | SPISETUP_OPTION5;

    txSent = (len < SPI_FIFO_DEPTH) ? len : SPI_FIFO_DEPTH;
    for (i = 0; i < txSent; i++)
        SPI4_REG(SPI_TXDATA) = pTx[i];
    SPI4_REG(SPI_SETUP) = setup | SPISETUP_OPTION5 | SPISETUP_RX_ENABLE |
                          SPISETUP_ENABLE2 | SPISETUP_TX_ENABLE;

    end = S5lReadTicks() + (UINT64)SPI_PROGRESS_US * TICKS_PER_US;
    while (rxDone < len)
    {
        st = SPI4_REG(SPI_STATUS);

        avail = SPISTATUS_RXLVL(st);
        if (avail > len - rxDone)
            avail = len - rxDone;
        for (i = 0; i < avail; i++)
        {
            b = SPI4_REG(SPI_RXDATA);
            if (pRx != NULL)
                pRx[rxDone] = (BYTE)b;
            rxDone++;
        }

        inFlight = txSent - rxDone;
        space = SPI_FIFO_DEPTH - SPISTATUS_TXLVL(st);
        if (space > SPI_FIFO_DEPTH - inFlight)
            space = SPI_FIFO_DEPTH - inFlight;
        push = len - txSent;
        if (push > space)
            push = space;
        for (i = 0; i < push; i++)
            SPI4_REG(SPI_TXDATA) = pTx[txSent + i];
        txSent += push;

        if (avail != 0 || push != 0)
        {
            end = S5lReadTicks() + (UINT64)SPI_PROGRESS_US * TICKS_PER_US;
            continue;
        }
        if (S5lReadTicks() > end)
        {
            ok = FALSE;
            break;
        }
    }

    if (pStatus != NULL)
        *pStatus = st;
    SPI4_REG(SPI_SETUP) = setup;
    SPI4_REG(SPI_STATUS) = SPISTATUS_CLEAR;

    DelayUs(Z2_CS_DELAY_US);
    PinOut(TOUCH_GPIO_CS, TRUE);
    DelayUs(delayAfterUs);
    return ok;
}

static void FormatBytes(const BYTE *p, UINT32 len, WCHAR *pszOut)
{
    static const WCHAR hex[] = L"0123456789abcdef";
    UINT32 i;

    for (i = 0; i < len; i++)
    {
        pszOut[i * 3]     = hex[p[i] >> 4];
        pszOut[i * 3 + 1] = hex[p[i] & 0xF];
        pszOut[i * 3 + 2] = L' ';
    }
    pszOut[(len > 0) ? (len * 3 - 1) : 0] = L'\0';
}

// index < 0: no number after the name
static void LogXfer(const WCHAR *pszWhat, int index, BOOL ok, const BYTE *pRx, UINT32 len, UINT32 status)
{
    WCHAR line[16 * 3 + 1];

    FormatBytes(pRx, len, line);
    if (index < 0)
        Debug(L"touchprobe:   %s: %s, received %s; data ready %d (status %08x)\r\n",
              pszWhat, ok ? L"ok" : L"TIMEOUT", line, PinLevel(TOUCH_GPIO_ATN), status);
    else
        Debug(L"touchprobe:   %s %d: %s, received %s; data ready %d (status %08x)\r\n",
              pszWhat, index, ok ? L"ok" : L"TIMEOUT", line, PinLevel(TOUCH_GPIO_ATN), status);
}

static void Step3SpiLink(void)
{
    static const BYTE prime[16] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
        0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
    };
    static const BYTE shortAck[2] = { 0x1a, 0xa1 };
    UINT32 oldSetup = SPI4_REG(SPI_SETUP), oldControl = SPI4_REG(SPI_CONTROL);
    UINT32 oldPin = SPI4_REG(SPI_PIN), oldDiv = SPI4_REG(SPI_CLKDIVIDER);
    UINT32 oldCs = PinLevel(TOUCH_GPIO_CS);
    UINT32 status, i;
    BYTE rx[16];
    BOOL ok;

    // controller set up as Linux's prepare_message does; CS inactive (high)
    SPI4_REG(SPI_SETUP) = SPISETUP_MODE3_8BIT;
    SPI4_REG(SPI_PIN) = 2;
    SPI4_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    SPI4_REG(SPI_CONTROL) = SPICTL_ENABLE;
    PinOut(TOUCH_GPIO_CS, TRUE);
    DelayUs(100);
    Debug(L"touchprobe: step 3: SPI4 setup %08x pin %08x, clkdiv %d (%d Hz); CS %d, reset %d, data ready %d\r\n",
          SPI4_REG(SPI_SETUP), SPI4_REG(SPI_PIN), Z2_DIV_NORMAL, 24000000 / Z2_DIV_NORMAL,
          PinLevel(TOUCH_GPIO_CS), PinLevel(TOUCH_GPIO_RESET), PinLevel(TOUCH_GPIO_ATN));

    // reset on 200 ms, dummy transfer, reset off, 15 ms
    PinOut(TOUCH_GPIO_RESET, TRUE);
    DelayUs(200000);
    memset(rx, 0, sizeof(rx));
    ok = SpiXfer(prime, rx, sizeof(prime), Z2_TX_DELAY_US, &status);
    LogXfer(L"dummy transfer in reset", -1, ok, rx, sizeof(prime), status);
    PinOut(TOUCH_GPIO_RESET, FALSE);
    DelayUs(15000);
    Debug(L"touchprobe:   15 ms after reset off: data ready %d\r\n", PinLevel(TOUCH_GPIO_ATN));

    // the dummy transfer again: should take the ready banner
    memset(rx, 0, sizeof(rx));
    ok = SpiXfer(prime, rx, sizeof(prime), Z2_TX_DELAY_US, &status);
    LogXfer(L"dummy transfer (banner)", -1, ok, rx, sizeof(prime), status);

    // short acks, as openiBoot reads them before the firmware upload
    for (i = 0; i < 4; i++)
    {
        memset(rx, 0, sizeof(rx));
        ok = SpiXfer(shortAck, rx, sizeof(shortAck), Z2_RX_DELAY_US, &status);
        LogXfer(L"short ack", (int)i, ok, rx, sizeof(shortAck), status);
        Debug(L"touchprobe:     ack value 0x%04x\r\n", (rx[0] << 8) | rx[1]);
    }

    // back as found: held in reset, SPI4 registers and CS as before
    PinOut(TOUCH_GPIO_RESET, TRUE);
    SPI4_REG(SPI_STATUS) = SPISTATUS_CLEAR;
    SPI4_REG(SPI_SETUP) = oldSetup;
    SPI4_REG(SPI_PIN) = oldPin;
    SPI4_REG(SPI_CLKDIVIDER) = oldDiv;
    SPI4_REG(SPI_CONTROL) = oldControl;
    PinOut(TOUCH_GPIO_CS, oldCs != 0);
    DelayUs(1000);
    Debug(L"touchprobe: step 3 done; reset %d, CS %d, data ready %d\r\n",
          PinLevel(TOUCH_GPIO_RESET), PinLevel(TOUCH_GPIO_CS), PinLevel(TOUCH_GPIO_ATN));
}

static void ProbeTouchReset(void)
{
    UINT32 g2 = S5L_REG(S5L_CLKGATES2_PA), g4 = S5L_REG(S5L_CLKGATES4_PA);
    UINT32 ldo = 0;
    DWORD step;

    // LDO 3 (touch, 1.8 V): enable = power chip register 0x10 bit 5,
    // voltage = 0x1c (n72-linux apple-n72-pmu-regulator.c)
    if (I2cRead(S5L_PMU_I2C_ADDR, 0x10, (BYTE *)&ldo, 1, &step) != PRB_OK)
    {
        Debug(L"touchprobe: steps 2/3 skipped: cannot read the power chip\r\n");
        return;
    }
    if ((ldo & 0x20) == 0)
    {
        Debug(L"touchprobe: steps 2/3 skipped: LDO 3 (touch supply) is off (reg 0x10 = %02x)\r\n", ldo);
        return;
    }
    Debug(L"touchprobe: LDO 3 on (reg 0x10 = %02x); data ready pin 0x%03x function %x\r\n",
          ldo, TOUCH_GPIO_ATN,
          (S5L_REG(S5L_GPIO_PORT_PA(10) + GPIO_CON) >> (4 * GPIO_PIN_BIT(TOUCH_GPIO_ATN))) & 0xF);

    if ((g2 & SPI4_GATES2_BIT) || (g4 & SPI4_GATES4_BIT))
    {
        Debug(L"touchprobe: step 3 skipped: SPI4 clock off\r\n");
        return;
    }

#ifdef TOUCHPROBE_STEP2
    ResetRun(L"A", FALSE);
    ResetRun(L"B", TRUE);
#endif
    Step3SpiLink();
}

// ---------------------------------------------------------------------------
// TouchProbe: called from OEMInit
//
void TouchProbe(void)
{
    Debug(L"touchprobe: start (step 1 read only)\r\n");
    ProbeGpio();
    ProbeSpi4();
    ProbePmu();
    ProbeTouchReset();
    Debug(L"touchprobe: done\r\n");
}

#else

void TouchProbe(void)
{
    return;
}

#endif // BSP_TOUCHPROBE
