//
// s5l8720_uart.h - polled UART0 (dock connector serial) helpers
//
// Shared by the OAL debug serial (debug.c) and the KITL serial driver
// (kitl\kitlSerDrv.c). Both run in kernel mode with the MMU on, so the
// registers are accessed through the uncached static mapping of the I/O
// block (S5L_IO_UA).
//
// The UART keeps the clock, speed and pins iBoot set up: the S5L8720 clock
// tree is not verified yet (openiBoot's UART clock gate number 0x29 is not
// even in its own gate table). S5lUart0Init only makes sure Rx and Tx are in
// polling mode. If the UART does not drain (no clock), output is dropped
// after a timeout instead of hanging the boot.
//
#ifndef __S5L8720_UART_H
#define __S5L8720_UART_H

#include <s5l8720.h>

#define UART0_REG(off)  S5L_REG(S5L_UART0_PA + (off))

// Loops of the Tx-empty poll before the UART is declared dead (several ms)
#define S5L_UART_TIMEOUT_LOOPS  200000

// Loops to wait after a write until UTRSTAT shows the byte as pending. One
// character takes 87 us at 115200 baud, so the bit must drop well within
// this; the limit only keeps a misbehaving UART from stalling every byte.
#define S5L_UART_PENDING_LOOPS  20000

// Each module (nk.exe, kitl.dll) has its own copy of this flag
static BOOL g_s5lUartDead = FALSE;

static __inline void S5lUart0Init(void)
{
    UINT32 ucon = UART0_REG(UART_UCON);

    if ((ucon & UCON_MODE_MASK) != UCON_MODE_POLL)
        UART0_REG(UART_UCON) = (ucon & ~UCON_MODE_MASK) | UCON_MODE_POLL;
}

static __inline void S5lUart0PutByte(UINT8 ch)
{
    UINT32 loops = 0;

    if (g_s5lUartDead)
        return;

    // wait for the transmitter (FIFO and shift register) to be empty
    while ((UART0_REG(UART_UTRSTAT) & UTRSTAT_TX_EMPTY) == 0)
    {
        if (++loops > S5L_UART_TIMEOUT_LOOPS)
        {
            g_s5lUartDead = TRUE;
            return;
        }
    }
    UART0_REG(UART_UTXH) = ch;

    // Wait until the UART shows the byte as pending (Tx-empty drops). The
    // status lags the write: once the kernel turns the caches on (after
    // OEMInit), the next call saw "empty" again, wrote over this byte and
    // the UART sent the second byte twice: every other character lost, the
    // rest doubled (seen on the device 2026-11-28).
    loops = 0;
    while ((UART0_REG(UART_UTRSTAT) & UTRSTAT_TX_EMPTY) != 0)
    {
        if (++loops > S5L_UART_PENDING_LOOPS)
            break;
    }
}

// Returns -1 when no byte is waiting.
static __inline int S5lUart0GetByte(void)
{
    if ((UART0_REG(UART_UTRSTAT) & UTRSTAT_RXBUF_RDY) == 0)
        return -1;
    (void)UART0_REG(UART_UERSTAT);              // read clears error status
    return (int)(UART0_REG(UART_URXH) & 0xFF);
}

#endif // __S5L8720_UART_H
