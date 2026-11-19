//
// s5l8702_uart.h - polled UART0 (dock connector serial) helpers
//
// Shared by the OAL debug serial (debug.c) and the KITL serial driver
// (kitl\kitlSerDrv.c). Both run in kernel mode with the MMU on, so the
// registers are accessed through the uncached static mapping of the I/O
// block (S5L_IO_UA). The same sequence runs with physical addresses in
// startup.s (EarlyUartInit) when early output is enabled.
//
// Sequence follows Rockbox uartc_open/uartc_port_open/serial_setup
// (firmware/target/arm/uc87xx.c, s5l8702/uart-s5l8702.c, ipod6g/serial-6g.c).
//
#ifndef __S5L8702_UART_H
#define __S5L8702_UART_H

#include <s5l8702.h>

#define UART0_REG(off)  S5L_REG(S5L_UART0_PA + (off))

// Ungate the UART clock, route GPIO 0.4/0.5 to UART0 and set 115200 8N1,
// FIFOs on, Rx and Tx enabled in polling mode, all UART interrupts off.
static __inline void S5lUart0Init(void)
{
    S5L_REG(S5L_PWRCON1_PA) &= ~S5L_PWRCON1_UART;

    S5L_REG(S5L_PCON_PA(0)) =
        (S5L_REG(S5L_PCON_PA(0)) & ~S5L_PCON0_UART0_MASK) | S5L_PCON0_UART0_FUNC;

    // reset the port
    UART0_REG(UART_UCON)    = 0;
    UART0_REG(UART_ULCON)   = 0;
    UART0_REG(UART_UMCON)   = 0;
    UART0_REG(UART_UTRSTAT) = 0xFFFFFFFF;       // clear interrupt status

    // FIFOs on (and reset), nRTS active
    UART0_REG(UART_UFCON)   = UFCON_FIFO_EN | UFCON_RX_FIFO_RST | UFCON_TX_FIFO_RST;
    UART0_REG(UART_UMCON)   = UMCON_RTS;

    // clock = ECLK (12 MHz), Rx/Tx disabled while configuring
    UART0_REG(UART_UCON)    = UCON_CLKSEL_ECLK;
    UART0_REG(UART_ULCON)   = ULCON_8N1;
    UART0_REG(UART_UBRDIV)  = UART_UBRDIV_115200;
    UART0_REG(UART_UBRCONRX) = UART_UBRCON_115200;
    UART0_REG(UART_UBRCONTX) = UART_UBRCON_115200;

    // enable Rx and Tx (polling mode, no interrupt enables)
    UART0_REG(UART_UCON)    = UCON_CLKSEL_ECLK | UCON_RX_MODE_INT | UCON_TX_MODE_INT;
}

static __inline void S5lUart0PutByte(UINT8 ch)
{
    while ((UART0_REG(UART_UTRSTAT) & UTRSTAT_TXBUF_EMPTY) == 0)
        ;
    UART0_REG(UART_UTXH) = ch;
}

// Returns -1 when no byte is waiting.
static __inline int S5lUart0GetByte(void)
{
    if ((UART0_REG(UART_UTRSTAT) & UTRSTAT_RXBUF_RDY) == 0)
        return -1;
    (void)UART0_REG(UART_UERSTAT);              // read clears error status
    return (int)(UART0_REG(UART_URXH) & 0xFF);
}

#endif // __S5L8702_UART_H
