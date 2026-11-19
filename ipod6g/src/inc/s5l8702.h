//
// s5l8702.h - Samsung S5L8702 (iPod classic) register definitions
//
// Values come from the Rockbox sources (firmware/export/s5l87xx.h,
// firmware/export/uc87xx.h, firmware/target/arm/s5l8702/*). Only the blocks
// the BSP uses so far are listed. Keep in sync with s5l8702.inc.
//
#ifndef __S5L8702_H
#define __S5L8702_H

//------------------------------------------------------------------------------
// Address map
//
// OEMAddrTable.inc maps:
//   VA 0x80000000 (cached) / 0xA0000000 (uncached) -> PA 0x08000000, 64 MB SDRAM
//   VA 0x84000000 (cached) / 0xA4000000 (uncached) -> PA 0x38000000, 128 MB I/O
//
#define S5L_DRAM_PA_BASE        0x08000000
#define S5L_DRAM_SIZE           0x04000000      // 64 MB

#define S5L_IO_PA_BASE          0x38000000
#define S5L_IO_SIZE             0x08000000      // 0x38000000-0x3FFFFFFF
#define S5L_IO_UA_BASE          0xA4000000      // uncached VA of S5L_IO_PA_BASE

#define S5L_IRAM0_PA_BASE       0x22000000      // 128 KB
#define S5L_IRAM1_PA_BASE       0x22020000      // 128 KB

// Uncached kernel VA of an I/O physical address (valid once the MMU is on)
#define S5L_IO_UA(pa)           ((UINT32)(pa) - S5L_IO_PA_BASE + S5L_IO_UA_BASE)
#define S5L_REG(pa)             (*(volatile UINT32 *)S5L_IO_UA(pa))

//------------------------------------------------------------------------------
// Clock controller (CLK_BASE)
//
#define S5L_CLK_PA              0x3C500000
#define S5L_PWRCON0_PA          (S5L_CLK_PA + 0x48)     // AHB clock gates (1 = gated)
#define S5L_PWRCON1_PA          (S5L_CLK_PA + 0x4C)     // APB clock gates (1 = gated)
#define S5L_RSTSR_PA            (S5L_CLK_PA + 0x54)     // reset status

// PWRCON1 (APB) gate bits
#define S5L_PWRCON1_TIMER       (1 << 5)
#define S5L_PWRCON1_UART        (1 << 9)
#define S5L_PWRCON1_GPIO        (1 << 12)

//------------------------------------------------------------------------------
// GPIO (port n at +n*0x20)
//
#define S5L_GPIO_PA             0x3CF00000
#define S5L_PCON_PA(n)          (S5L_GPIO_PA + ((n) << 5))
#define S5L_PDAT_PA(n)          (S5L_GPIO_PA + ((n) << 5) + 0x04)

// PCON0 bits 23:16 = GPIO 0.4/0.5 function; 0x22 = UART0 TXD/RXD
#define S5L_PCON0_UART0_MASK    0x00FF0000
#define S5L_PCON0_UART0_FUNC    0x00220000

//------------------------------------------------------------------------------
// Vectored interrupt controllers (two PL192-style VICs, IRQ 0-31 and 32-63)
//
#define S5L_VIC0_PA             0x38E00000
#define S5L_VIC1_PA             0x38E01000
#define S5L_VICEDGE_PA          0x38E02000

#define VIC_IRQSTATUS           0x000
#define VIC_FIQSTATUS           0x004
#define VIC_RAWINTR             0x008
#define VIC_INTSELECT           0x00C
#define VIC_INTENABLE           0x010
#define VIC_INTENCLEAR          0x014
#define VIC_SOFTINT             0x018
#define VIC_SOFTINTCLEAR        0x01C
#define VIC_VECTADDR0           0x100
#define VIC_VECTPRIORITY0       0x200
#define VIC_ADDRESS             0xF00

#define S5L_VIC0EDGE1_PA        (S5L_VICEDGE_PA + 0x08)
#define S5L_VIC1EDGE1_PA        (S5L_VICEDGE_PA + 0x0C)

// IRQ numbers
#define S5L_IRQ_EXT0            0
#define S5L_IRQ_TIMER32         7       // timers E-H
#define S5L_IRQ_TIMER           8       // timers A-D
#define S5L_IRQ_LCD             14
#define S5L_IRQ_DMAC0           16
#define S5L_IRQ_DMAC1           17
#define S5L_IRQ_USB_FUNC        19
#define S5L_IRQ_I2C0            21
#define S5L_IRQ_I2C1            22
#define S5L_IRQ_WHEEL           23
#define S5L_IRQ_UART0           24
#define S5L_IRQ_ATA             29
#define S5L_IRQ_MMC             44      // SDCI (CE-ATA)
#define S5L_IRQ_MAX             64

//------------------------------------------------------------------------------
// Timers
//
// Timer B: 16-bit, used for the 1 ms system tick (ECLK 12 MHz / 16 / 75 = 10 kHz)
// Timer E: 32-bit, free-running 1 MHz counter (performance counter)
//
#define S5L_TIMER_PA            0x3C700000

#define S5L_TBCON_PA            (S5L_TIMER_PA + 0x20)
#define S5L_TBCMD_PA            (S5L_TIMER_PA + 0x24)
#define S5L_TBDATA0_PA          (S5L_TIMER_PA + 0x28)
#define S5L_TBDATA1_PA          (S5L_TIMER_PA + 0x2C)
#define S5L_TBPRE_PA            (S5L_TIMER_PA + 0x30)
#define S5L_TBCNT_PA            (S5L_TIMER_PA + 0x34)

#define S5L_TECON_PA            (S5L_TIMER_PA + 0xA0)
#define S5L_TECMD_PA            (S5L_TIMER_PA + 0xA4)
#define S5L_TEDATA0_PA          (S5L_TIMER_PA + 0xA8)
#define S5L_TEDATA1_PA          (S5L_TIMER_PA + 0xAC)
#define S5L_TEPRE_PA            (S5L_TIMER_PA + 0xB0)
#define S5L_TECNT_PA            (S5L_TIMER_PA + 0xB4)

#define S5L_TSTAT_PA            (S5L_TIMER_PA + 0x118)

// TxCMD
#define TCMD_EN                 (1 << 0)
#define TCMD_CLR                (1 << 1)

// TxCON
#define TCON_INT0_EN            (1 << 12)
#define TCON_INT1_EN            (1 << 13)
#define TCON_CS(n)              ((n) << 8)      // 0 = /2, 1 = /4, 2 = /16, 3 = /64, 4 = /1 (32-bit timers)
#define TCON_ECLK               (1 << 6)        // clock from ECLK (OSC0, 12 MHz)
#define TCON_MODE_INTERVAL      (0 << 4)
#define TCON_INT_PENDING        (0x7 << 16)     // TxCON & (TxCON >> 4) & 0x7000 = pending ints

#define S5L_OSC0_HZ             12000000

//------------------------------------------------------------------------------
// Watchdog
//
#define S5L_WDT_PA              0x3C800000
#define S5L_WDTCON_PA           (S5L_WDT_PA + 0x00)
#define S5L_WDTCNT_PA           (S5L_WDT_PA + 0x04)
#define S5L_WDTCON_RESET        0x00100000      // writing this resets the SoC

//------------------------------------------------------------------------------
// UART controller (4 ports at +n*0x4000). Port 0 = dock connector serial.
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
#define UART_UBRDIV             0x28
#define UART_UBRCONTX           0x34
#define UART_UBRCONRX           0x38

#define ULCON_8N1               0x03

#define UCON_RX_MODE_INT        (1 << 0)        // Rx enabled (interrupt/polling)
#define UCON_TX_MODE_INT        (1 << 2)        // Tx enabled (interrupt/polling)
#define UCON_RX_TOUT_EN         (1 << 7)
#define UCON_CLKSEL_ECLK        (1 << 10)       // 12 MHz OSC0
#define UCON_RX_TOUT_INT        (1 << 11)
#define UCON_RX_INT             (1 << 12)
#define UCON_TX_INT             (1 << 13)
#define UCON_ERR_INT            (1 << 14)

#define UFCON_FIFO_EN           (1 << 0)
#define UFCON_RX_FIFO_RST       (1 << 1)
#define UFCON_TX_FIFO_RST       (1 << 2)

#define UMCON_RTS               (1 << 0)

#define UTRSTAT_RXBUF_RDY       (1 << 0)
#define UTRSTAT_TXBUF_EMPTY     (1 << 1)
#define UTRSTAT_TX_EMPTY        (1 << 2)

// 115200 baud from ECLK 12 MHz (Rockbox serial-6g.c BRDATA_115200):
// UBRDIV = 6, fine-tune 0xFFFFFF on Tx and Rx -> 114286 baud (-0.8%)
#define UART_UBRDIV_115200      6
#define UART_UBRCON_115200      0x00FFFFFF

#endif // __S5L8702_H
