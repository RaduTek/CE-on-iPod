//
// ipodwhl.h - the iPod's click wheel under Windows CE: what the driver
// (ipodwhl.dll, device WHL1:) and the program that turns its events into
// keys (whlkeys.exe) share
//
#ifndef IPODWHL_H
#define IPODWHL_H

#include <whlcore.h>

#define IPODWHL_VERSION     1
#define IPODWHL_DEVICE      L"WHL1:"

// Reading the device gives whole WHL_EVENT records (whlcore.h): a button
// pressed or let go, the wheel one step on. A read waits up to 100 ms for
// the first one and returns 0 bytes when there was none.

#define IOCTL_IPODWHL_GET_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x990, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;
    DWORD   mapped;         // the registers could be mapped
    DWORD   running;        // the thread that looks at the controller runs
    DWORD   gateClosed;     // looks at which the controller had no clock (it was left alone then)
    DWORD   pwrcon1;        // the clock switches, bit 1 = the wheel's (0 = clock on)
    DWORD   pcon14;         // the wheel's pins
    DWORD   looks;
    DWORD   flagsSeen;      // every flag bit seen
    DWORD   cleared;        // times flags were written back
    DWORD   reports;        // times the report read different from before
    DWORD   events;         // events made of them
    DWORD   dropped;        // events nobody fetched in time
    DWORD   lastRx;
    DWORD   buttons;        // held now (WHL_BTN_ bits)
    DWORD   touched;
    DWORD   pos;
    DWORD   maxGapMs;       // longest time between two looks
} IPODWHL_STATUS;

// whlkeys.exe's window (class IpodWheelKeys) takes this message from a program:
// wParam 1 = turn the wheel into volume keys, 0 = into arrow keys again;
// lParam = the asking program's window. Volume keys are sent only while that
// window exists and is the one in front; at any other time the wheel is the
// arrow keys, whatever was asked.
#define WHLKEYS_CLASS       L"IpodWheelKeys"
#define WM_WHLKEYS_MODE     (WM_APP + 2)

#endif
