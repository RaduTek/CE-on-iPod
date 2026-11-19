//
// kitl.c - KITL startup for the ipod6g BSP (serial KITL on UART0)
//
// Uses the stock platform-common KITL library (oal_kitl.lib), following the
// FORCE_KITL_SERIAL path of the Microsoft 3DS_iMX27 BSP (Src\kitl\kitl.c).
//
// The device is declared InterfaceTypeUndefined so OALKitlFindDevice takes
// the address as-is (no IOCTL_HAL_DDK_CALL bus translation, which this BSP's
// small OEMIoControl does not implement). The serial driver ignores the
// address anyway and uses the static uncached mapping of UART0.
//
#include <windows.h>
#include <oal.h>
#include <kitl.h>
#include <oal_kitl.h>

#include <bsp.h>

#define KITL_SERIAL_BAUD    115200

extern OAL_KITL_SERIAL_DRIVER g_kitlS5lUart0;   // kitlSerDrv.c

//------------------------------------------------------------------------------
// KITL device table
//
OAL_KITL_DEVICE g_kitlDevices[] = {
    {
        L"S5LUART0", InterfaceTypeUndefined, S5L_UART0_PA, 0,
        OAL_KITL_TYPE_SERIAL, &g_kitlS5lUart0
    },
    {
        NULL, 0, 0, 0, 0, NULL
    }
};

// ---------------------------------------------------------------------------
// OEMKitlStartup: called from OEMInit via KITLIoctl(IOCTL_KITL_STARTUP)
//
BOOL OEMKitlStartup(void)
{
    BOOL rc;
    OAL_KITL_ARGS args;
    // OALKitlInit copies OAL_KITL_ID_SIZE bytes from the name
    CHAR szDeviceId[OAL_KITL_ID_SIZE] = "iPodCE";

    OALMSG(OAL_KITL && OAL_FUNC, (L"+OEMKitlStartup\r\n"));

    memset(&args, 0, sizeof(args));
    args.flags              = OAL_KITL_FLAGS_ENABLED | OAL_KITL_FLAGS_POLL;
    args.devLoc.IfcType     = InterfaceTypeUndefined;
    args.devLoc.BusNumber   = 0;
    args.devLoc.LogicalLoc  = S5L_UART0_PA;
    args.devLoc.Pin         = S5L_IRQ_UART0;
    args.baudRate           = KITL_SERIAL_BAUD;
    args.dataBits           = 8;
    args.stopBits           = 1;
    args.parity             = 0;

    rc = OALKitlInit(szDeviceId, &args, g_kitlDevices);

    OALMSG(OAL_KITL && OAL_FUNC, (L"-OEMKitlStartup(rc = %d)\r\n", rc));
    return rc;
}

// ---------------------------------------------------------------------------
// OALGetTickCount: ms since boot, for the KITL library timeouts.
//
// kitl.dll has its own copy of globals, so read the hardware directly:
// timer E is a free-running 1 MHz counter (started by the OAL in
// OALTimerInit and left running by the Rockbox bootloader before that).
//
UINT32 OALGetTickCount(void)
{
    return S5L_REG(S5L_TECNT_PA) / 1000;
}
