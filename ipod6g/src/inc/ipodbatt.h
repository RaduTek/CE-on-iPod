//
// ipodbatt.h - what the iPod's battery and clock driver (battdrvr.dll,
// "BAT1:") says about itself, for the power panel
//
#ifndef IPODBATT_H
#define IPODBATT_H

#include <winioctl.h>
#include <ipodi2c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODBATT_VERSION    1

// DeviceIoControl on "BAT1:" with this code returns an IPODBATT_STATUS
#define IOCTL_IPODBATT_GET_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x960, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;
    // the clock, once at start-up
    DWORD   clockRead;      // the power chip's clock was read
    BYTE    clockRaw[8];    // second, minute, hour, weekday, day, month, year (BCD)
    DWORD   clockValid;     // it was a date
    DWORD   timeAccepted;   // Windows took it
    DWORD   timeError;      // Windows' error code when it did not
    // the battery
    DWORD   asked;          // times Windows asked for the status
    DWORD   measures;       // measurements that gave a result
    DWORD   fails;          // measurements that did not
    DWORD   mv;             // the last result
    DWORD   percent;
    DWORD   mbcs1;          // the chip's charger status: bit 0 USB power, bit 2 adapter
    DWORD   charging;       // the charge pin
    IPODI2C_STATE bus;      // the bus layer's own account
} IPODBATT_STATUS;

#ifdef __cplusplus
}
#endif

#endif
