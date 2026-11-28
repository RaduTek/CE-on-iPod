//
// kitl.c - KITL startup for the ipodtouch2g BSP (serial KITL on UART0)
//
// Uses the stock platform-common KITL library (oal_kitl.lib), following the
// FORCE_KITL_SERIAL path of the Microsoft 3DS_iMX27 BSP (Src\kitl\kitl.c),
// as the ipod6g BSP does.
//
// The device is declared InterfaceTypeUndefined so OALKitlFindDevice takes
// the address as-is (no IOCTL_HAL_DDK_CALL bus translation, which this BSP's
// small OEMIoControl does not implement). The serial driver ignores the
// address anyway and uses the static uncached mapping of UART0. The UART
// runs at the speed iBoot set up; Platform Builder must use the same
// (115200 assumed).
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
    CHAR szDeviceId[OAL_KITL_ID_SIZE] = "iPodtouchCE";

    OALMSG(OAL_KITL && OAL_FUNC, (L"+OEMKitlStartup\r\n"));

    memset(&args, 0, sizeof(args));
    args.flags              = OAL_KITL_FLAGS_ENABLED | OAL_KITL_FLAGS_POLL;
    args.devLoc.IfcType     = InterfaceTypeUndefined;
    args.devLoc.BusNumber   = 0;
    args.devLoc.LogicalLoc  = S5L_UART0_PA;
    args.devLoc.Pin         = 0;                // polled: no IRQ
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
// kitl.dll has its own copy of globals, so read the hardware directly: the
// 64-bit tick counter runs from iBoot on (s5l8720.h).
//
UINT32 OALGetTickCount(void)
{
    static UINT32 ticksPerMs = 0;

    if (ticksPerMs == 0)
        ticksPerMs = S5lTicksPerSec() / 1000;

    return (UINT32)(S5lReadTicks() / ticksPerMs);
}
