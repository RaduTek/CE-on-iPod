//
// s5l8720.h - Samsung S5L8720 (iPod touch 2G) register definitions
//
// Values come from the openiBoot sources (plat-s5l8720/, iDroid project),
// used as documentation. openiBoot's S5L8720 port is incomplete, so treat
// every value here as unverified until the hand-over probe (startup.s) or a
// driver has confirmed it on the device. Keep in sync with s5l8720.inc.
//
#ifndef __S5L8720_H
#define __S5L8720_H

//------------------------------------------------------------------------------
// Address map
//
// OEMAddrTable.inc maps:
//   VA 0x80000000 (cached) / 0xA0000000 (uncached) -> PA 0x08000000, 128 MB DRAM
//   VA 0x88000000 (cached) / 0xA8000000 (uncached) -> PA 0x38000000, 128 MB I/O
//
#define S5L_DRAM_PA_BASE        0x08000000
#define S5L_DRAM_SIZE           0x08000000      // 128 MB
#define S5L_DRAM_UA_BASE        0xA0000000      // uncached VA of S5L_DRAM_PA_BASE

// Top of DRAM kept out of CE's RAM (config.bib): openiBoot's framebuffer
// address, probably also iBoot's
#define S5L_FB_RESERVED_PA      0x0FC00000
#define S5L_FB_RESERVED_SIZE    0x00400000

#define S5L_IO_PA_BASE          0x38000000
#define S5L_IO_SIZE             0x08000000      // 0x38000000-0x3FFFFFFF
#define S5L_IO_UA_BASE          0xA8000000      // uncached VA of S5L_IO_PA_BASE

// Uncached kernel VA of an I/O physical address (valid once the MMU is on)
#define S5L_IO_UA(pa)           ((UINT32)(pa) - S5L_IO_PA_BASE + S5L_IO_UA_BASE)
#define S5L_REG(pa)             (*(volatile UINT32 *)S5L_IO_UA(pa))

// Cached / uncached kernel VA of a DRAM physical address
#define S5L_DRAM_CA_BASE        0x80000000      // cached VA of S5L_DRAM_PA_BASE
#define S5L_DRAM_CA(pa)         ((UINT32)(pa) - S5L_DRAM_PA_BASE + S5L_DRAM_CA_BASE)
#define S5L_DRAM_UA(pa)         ((UINT32)(pa) - S5L_DRAM_PA_BASE + S5L_DRAM_UA_BASE)

//------------------------------------------------------------------------------
// Chip ID
//
#define S5L_CHIPID_PA           0x3D100000
#define S5L_CHIPINFO_PA         (S5L_CHIPID_PA + 0x08)
#define CHIPINFO_SECURITY_EPOCH(x)  ((x) & 1)

// Base (crystal) clock as openiBoot derives it from the security epoch
#define S5L_BASE_HZ_EPOCH1      24000000
#define S5L_BASE_HZ_EPOCH0      12000000

//------------------------------------------------------------------------------
// Clock controller (openiBoot plat-s5l8720/includes/hardware/clock.h)
//
#define S5L_CLK_PA              0x3C500000
#define S5L_CLKCONFIG0_PA       (S5L_CLK_PA + 0x00)
#define S5L_CLKCONFIG1_PA       (S5L_CLK_PA + 0x04)
#define S5L_CLKCONFIG2_PA       (S5L_CLK_PA + 0x08)
#define S5L_PLL0CON_PA          (S5L_CLK_PA + 0x20)
#define S5L_PLLMODE_PA          (S5L_CLK_PA + 0x44)
#define S5L_CLKGATES0_PA        (S5L_CLK_PA + 0x48)     // 1 = gated (off)
#define S5L_CLKGATES1_PA        (S5L_CLK_PA + 0x4C)
#define S5L_CLKGATES2_PA        (S5L_CLK_PA + 0x58)
#define S5L_CLKGATES3_PA        (S5L_CLK_PA + 0x68)
#define S5L_CLKGATES4_PA        (S5L_CLK_PA + 0x6C)

//------------------------------------------------------------------------------
// Vectored interrupt controllers (two PL192, IRQ 0-31 and 32-63)
//
#define S5L_VIC0_PA             0x38E00000
#define S5L_VIC1_PA             0x38E01000
#define S5L_EDGEIC_PA           0x38E02000

#define VIC_IRQSTATUS           0x000
#define VIC_FIQSTATUS           0x004
#define VIC_RAWINTR             0x008
#define VIC_INTSELECT           0x00C
#define VIC_INTENABLE           0x010
#define VIC_INTENCLEAR          0x014
#define VIC_SOFTINT             0x018
#define VIC_SOFTINTCLEAR        0x01C
#define VIC_SWPRIORITYMASK      0x024
#define VIC_VECTADDR0           0x100
#define VIC_ADDRESS             0xF00

#define EDGEIC_CONFIG0          0x000
#define EDGEIC_CONFIG1          0x004

// IRQ numbers (openiBoot)
#define S5L_IRQ_TIMER           7       // all timers (openiBoot handles timers 4-7 here)
#define S5L_IRQ_DMAC0           16
#define S5L_IRQ_DMAC1           17
#define S5L_IRQ_MAX             64

//------------------------------------------------------------------------------
// Timers (same block layout as the S5L8702: timer n at +0x20*n for n = 0-3,
// timers 4-7 at 0xA0, 0xC0, 0xE0, 0x100)
//
#define S5L_TIMER_PA            0x3C700000
#define S5L_TIMERn_PA(n)        (S5L_TIMER_PA + (((n) < 4) ? ((n) * 0x20) : (0xA0 + ((n) - 4) * 0x20)))

#define TIMER_CON               0x00
#define TIMER_CMD               0x04
#define TIMER_DATA0             0x08
#define TIMER_DATA1             0x0C
#define TIMER_PRE               0x10
#define TIMER_CNT               0x14

// TIMER_CMD values (openiBoot: TIMER_STATE_*)
#define TCMD_STOP               0
#define TCMD_START              1
#define TCMD_UPDATE             2       // load the new settings

// TIMER_CON bits (openiBoot timer_init)
#define TCON_INT_EN             0x7000  // INT0, INT1, overflow interrupts
#define TCON_DIV(code)          ((code) << 8)   // 0 = /2, 1 = /4, 2 = /16, 3 = /64, 4 = /1
#define TCON_FIXEDCLK           (1 << 6)        // "option6": fixed (crystal) clock

// 64-bit free-running counter (openiBoot "ticks", TicksPerSec = base clock / 2)
#define S5L_TICKSHIGH_PA        (S5L_TIMER_PA + 0x80)
#define S5L_TICKSLOW_PA         (S5L_TIMER_PA + 0x84)
#define S5L_TIMERCTRL_PA        (S5L_TIMER_PA + 0x88)   // openiBoot TIMER_UNKREG0
#define S5L_TIMERUNK1_PA        (S5L_TIMER_PA + 0x8C)
#define S5L_TIMERUNK2_PA        (S5L_TIMER_PA + 0x90)
#define S5L_TIMERUNK3_PA        (S5L_TIMER_PA + 0x94)
#define S5L_TIMERUNK4_PA        (S5L_TIMER_PA + 0x98)
#define S5L_TIMERIRQLATCH_PA    (S5L_TIMER_PA + 0x118)
#define S5L_TIMERIRQSTAT_PA     (S5L_TIMER_PA + 0x10000)

// TIMERIRQSTAT: timer n (4-7) reports in bits 8*(7-n) .. 8*(7-n)+7
#define TIMERIRQSTAT_SHIFT(n)   (8 * (7 - (n)))
#define TIMERIRQSTAT_SPECIAL0   0x01000000      // openiBoot TIMER_SPECIALTIMER_BIT0
#define TIMERIRQSTAT_SPECIAL1   0x02000000

//------------------------------------------------------------------------------
// Watchdog
//
#define S5L_WDT_PA              0x3C800000
#define S5L_WDTCON_PA           (S5L_WDT_PA + 0x00)
#define S5L_WDTCON_RESET        0x00100000      // writing this resets the SoC (openiBoot DebugReboot)

//------------------------------------------------------------------------------
// UART controller (5 ports at +n*0x4000). Port 0 = openiBoot's console and,
// presumably, the dock connector serial.
//
// Unlike the S5L8702 this UART has UBAUD (divider bits 15:0, sample rate
// bits 17:16) instead of UBRDIV/UBRCON. The OAL does not program the speed:
// it keeps what iBoot set up (115200 8N1 assumed).
//
#define S5L_UART0_PA            0x3CC00000

#define UART_ULCON              0x00
#define UART_UCON               0x04
#define UART_UFCON              0x08
#define UART_UMCON              0x0C
#define UART_UTRSTAT            0x10
#define UART_UERSTAT            0x14
#define UART_UFSTAT             0x18
#define UART_UMSTAT             0x1C
#define UART_UTXH               0x20
#define UART_URXH               0x24
#define UART_UBAUD              0x28
#define UART_UDIVSLOT           0x2C

#define UCON_MODE_MASK          0x0F            // Rx mode bits 1:0, Tx mode bits 3:2
#define UCON_MODE_POLL          0x05            // Rx and Tx: interrupt/polling mode

#define UTRSTAT_RXBUF_RDY       (1 << 0)
#define UTRSTAT_TXBUF_EMPTY     (1 << 1)
#define UTRSTAT_TX_EMPTY        (1 << 2)

//------------------------------------------------------------------------------
// Display controller (openiBoot plat-s5l8720/clcd.c, createWindow)
//
#define S5L_CLCD_PA             0x38900000
#define CLCD_W0_FORMAT          0x20    // bits 11:8: 7 = RGB888 (32 bpp), 4 = RGB565
#define CLCD_W0_ADDR            0x24    // framebuffer physical address
#define CLCD_W0_STRIDE          0x28    // line length in pixels
#define CLCD_W0_SIZE            0x30    // (width << 16) | height

#define CLCD_FORMAT_RGB888      7
#define CLCD_FORMAT_RGB565      4

//------------------------------------------------------------------------------
// GPIO (openiBoot plat-s5l8720/gpio.c; ioreg gpio@4F00000: 15 ports,
// "fsel-offset" 0x1e0). Pin numbers are 0xPPB: port PP, bit B.
//
#define S5L_GPIO_PA             0x3CF00000
#define S5L_GPIO_NUMPORTS       15
#define S5L_GPIO_PORT_PA(p)     (S5L_GPIO_PA + (p) * 0x20)
#define GPIO_CON                0x00    // 4 bits per pin
#define GPIO_DAT                0x04
#define GPIO_PUD1               0x08
#define GPIO_PUD2               0x0C
#define S5L_GPIO_FSEL_PA        (S5L_GPIO_PA + 0x1E0)
#define GPIO_PIN_PORT(pin)      (((pin) >> 8) & 0x1F)
#define GPIO_PIN_BIT(pin)       ((pin) & 0x7)

// GPIO interrupt controller ("probably a part of the system controller")
#define S5L_GPIOIC_PA           0x39700000
#define GPIOIC_INTLEVEL         0x80    // + 4 * group
#define GPIOIC_INTSTAT          0xA0
#define GPIOIC_INTEN            0xC0
#define GPIOIC_INTTYPE          0xE0
#define GPIOIC_NUMGROUPS        7

//------------------------------------------------------------------------------
// I2C controller 0 (same register set as the S5L8702; openiBoot i2c.c).
// The power chip (Dialog D1759, ioreg pmu@73) is at 7-bit address 0x73.
//
#define S5L_I2C0_PA             0x3C600000
#define I2C_IICCON              0x00
#define I2C_IICSTAT             0x04
#define I2C_IICDS               0x0C
#define I2C_IICUNK10            0x10    // not 0 while the controller is busy with a register access
#define I2C_IICSTA2             0x20    // bits 8 and 13: a byte (or start/stop) has finished
#define S5L_PMU_I2C_ADDR        0x73

// openiBoot clock gate 7 (I2C0): gates1 bit 4, gates4 bit 11 (1 = off)
#define I2C0_GATES1_BIT         0x00000010
#define I2C0_GATES4_BIT         0x00000800

//------------------------------------------------------------------------------
// SPI controller 4: the touch controller (ioreg spi4@6100000, openiBoot
// spi.h). Register 0x20 (RXDATA) pops the receive FIFO when read.
//
#define S5L_SPI4_PA             0x3E100000
#define SPI_CONTROL             0x00
#define SPI_SETUP               0x04
#define SPI_STATUS              0x08
#define SPI_PIN                 0x0C
#define SPI_TXDATA              0x10
#define SPI_RXDATA              0x20
#define SPI_CLKDIVIDER          0x30
#define SPI_CNT                 0x34
#define SPI_IDD                 0x38
#define SPI_TXBUFFERLEN         0x4C

// openiBoot clock gate 0x12 (SPI4): gates2 bit 4, gates4 bit 20 (1 = off)
#define SPI4_GATES2_BIT         0x00000010
#define SPI4_GATES4_BIT         0x00100000

// openiBoot gpio_custom_io: FSEL = port << 16 | bit << 8 | mode
#define GPIO_FSEL_VALUE(pin, mode)  ((GPIO_PIN_PORT(pin) << 16) | (GPIO_PIN_BIT(pin) << 8) | ((mode) & 0xF))
#define GPIO_FSEL_OUT_LOW       0xE
#define GPIO_FSEL_OUT_HIGH      0xF

// SPI SETUP bits for mode 3 (openiBoot spi_set_baud(..., 1, 1, 1))
#define SPISETUP_LAST_CLOCK_EDGE_MISSING    (1 << 1)
#define SPISETUP_IS_ACTIVE_LOW              (1 << 2)

// Touch controller pins (ioreg multi-touch@0 and spi4@6100000; n72-linux
// s5l8720-n72.dts: reset GPIO 86 active-high, CS GPIO 87 active-low,
// data-ready GPIO interrupt 0x6d level-low; GPIO interrupt = pin + 24)
#define TOUCH_GPIO_RESET        0x0A06  // GPIO 86, high = held in reset
#define TOUCH_GPIO_CS           0x0A07  // GPIO 87, low = selected
#define TOUCH_GPIO_ATN          0x0A05  // GPIO 85, data ready, low = pending
#define TOUCH_GPIOINT_ATN       0x6D    // GPIO interrupt 109 = pin 85 + 24: group 3, bit 13

//------------------------------------------------------------------------------
// Memory controller remap (openiBoot entry.sx; not used by the BSP)
//
#define S5L_MIU_REMAP_PA        0x3E00000C

//------------------------------------------------------------------------------
// Helpers shared by the OAL and kitl.dll (both read the hardware directly)
//

// Rate of the 64-bit tick counter, as openiBoot computes it
static __inline UINT32 S5lTicksPerSec(void)
{
    UINT32 info = S5L_REG(S5L_CHIPINFO_PA);
    return (CHIPINFO_SECURITY_EPOCH(info) ? S5L_BASE_HZ_EPOCH1 : S5L_BASE_HZ_EPOCH0) / 2;
}

// Consistent read of the 64-bit tick counter
static __inline UINT64 S5lReadTicks(void)
{
    UINT32 high, low, high2;

    do
    {
        high = S5L_REG(S5L_TICKSHIGH_PA);
        low = S5L_REG(S5L_TICKSLOW_PA);
        high2 = S5L_REG(S5L_TICKSHIGH_PA);
    } while (high != high2);

    return ((UINT64)high << 32) | low;
}

#endif // __S5L8720_H
