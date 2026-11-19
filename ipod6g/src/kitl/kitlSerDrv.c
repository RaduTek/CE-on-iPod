//
// kitlSerDrv.c - KITL serial driver for UART0 (dock connector), ipod6g BSP
//
// Implements OAL_KITL_SERIAL_DRIVER (platform\common\src\inc\oal_kitl.h) for
// the polled S5L8702 UART. The packet framing ('kITL' header, checksums) is
// done by the platform-common kitlserial.c in oal_kitl.lib.
//
// Modelled on SMDK6410\SRC\KITL\kitlser.c (Samsung UART) and the CEonPi
// kitlSerDrv.c. No flow control: the dock connector only carries TX/RX/GND.
//
#include <windows.h>
#include <oal.h>
#include <kitl.h>
#include <oal_kitl.h>

#include <bsp.h>
#include <s5l8702_uart.h>

// Chunk size the KITL framework uses for send/receive calls
#define KITL_SERIAL_BEST_SIZE   64

// ---------------------------------------------------------------------------
// S5lKitlInit
//
static BOOL S5lKitlInit(KITL_SERIAL_INFO *pInfo)
{
    // pInfo->pAddress is the physical address (InterfaceTypeUndefined);
    // the registers are reached through the static uncached mapping instead.
    pInfo->bestSize = KITL_SERIAL_BEST_SIZE;

    S5lUart0Init();

    KITLOutputDebugString("[KITL] S5L UART0 initialized, %d baud\r\n", pInfo->baudRate);
    return TRUE;
}

static VOID S5lKitlDeinit(void)
{
    return;
}

// ---------------------------------------------------------------------------
// S5lKitlSend: blocking write of the whole buffer
//
static UINT16 S5lKitlSend(UINT8 *pData, UINT16 size)
{
    UINT16 i;

    for (i = 0; i < size; i++)
        S5lUart0PutByte(pData[i]);

    return size;
}

// ---------------------------------------------------------------------------
// S5lKitlRecv: non-blocking, returns the number of bytes read
//
static UINT16 S5lKitlRecv(UINT8 *pData, UINT16 size)
{
    UINT16 count = 0;
    int ch;

    while (count < size)
    {
        ch = S5lUart0GetByte();
        if (ch < 0)
            break;
        pData[count++] = (UINT8)ch;
    }
    return count;
}

// Polled mode: interrupt and power hooks do nothing
static VOID S5lKitlEnableInts(void)
{
    return;
}

static VOID S5lKitlDisableInts(void)
{
    return;
}

static VOID S5lKitlPowerOff(void)
{
    return;
}

static VOID S5lKitlPowerOn(void)
{
    S5lUart0Init();
}

//------------------------------------------------------------------------------
// Driver table (order as in OAL_KITL_SERIAL_DRIVER)
//
OAL_KITL_SERIAL_DRIVER g_kitlS5lUart0 = {
    S5lKitlInit,            // pfnInit
    S5lKitlDeinit,          // pfnDeinit
    S5lKitlSend,            // pfnSend
    NULL,                   // pfnSendComplete
    S5lKitlRecv,            // pfnRecv
    S5lKitlEnableInts,      // pfnEnableInts
    S5lKitlDisableInts,     // pfnDisableInts
    S5lKitlPowerOff,        // pfnPowerOff
    S5lKitlPowerOn,         // pfnPowerOn
    NULL                    // pfnFlowControl
};
