//
// ipodi2c.h - the iPod's I2C bus 0 under Windows CE: stream driver "I2C1:"
// (src\drivers\ipodi2c, ipodi2c.dll)
//
// The driver follows Microsoft's I2C interface (public\common\oak\inc\i2c.h):
// open it with I2cOpen(L"I2C1:"), choose the chip with I2cSetSlaveAddress
// (7-bit address), then I2cRead / I2cWrite at a register. Only drivers in the
// kernel's device manager may open it, as with Microsoft's own I2C driver.
//
// On top of Microsoft's interface:
//   - only the chips below are reachable, and the power chip only takes
//     writes to the registers its battery measurement uses (see the table
//     in ipodi2c.c); anything else is refused with ERROR_ACCESS_DENIED;
//   - IOCTL_IPODI2C_GET_STATE returns the driver's account (IPODI2C_STATE).
//
#ifndef IPODI2C_H
#define IPODI2C_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODI2C_VERSION         2
#define IPODI2C_DEVICE          L"I2C1:"

// the chips on bus 0, 7-bit addresses (Rockbox writes them shifted: 0xE6, 0x94)
#define IPODI2C_ADDR_PMU        0x73    // PCF50635 power chip: clock, charger, battery measurement
#define IPODI2C_ADDR_CODEC      0x4A    // CS42L55 audio codec

#define IPODI2C_MAX_LENGTH      64      // bytes per read or write

// error codes (lastCode); lastStep says where
#define IPODI2C_OK              0
#define IPODI2C_E_NOT_MAPPED    1       // no registers
#define IPODI2C_E_PINS          2       // the bus pins are not set up as Rockbox sets them
#define IPODI2C_E_NOT_READY     3       // the controller never became ready for a register access
#define IPODI2C_E_NO_END        4       // a byte never finished on the bus
#define IPODI2C_E_NO_ANSWER     5       // the chip did not acknowledge
#define IPODI2C_E_PARAM         6
#define IPODI2C_E_REFUSED       7       // chip or register not allowed (no bus activity)

// steps (lastStep)
#define IPODI2C_STEP_CHECK      1
#define IPODI2C_STEP_ADDRESS    2       // start + chip address, to send the register number
#define IPODI2C_STEP_REGISTER   3       // the register number
#define IPODI2C_STEP_STOP1      4
#define IPODI2C_STEP_READ       5       // start + chip address, to read
#define IPODI2C_STEP_DATA       6       // the data bytes read
#define IPODI2C_STEP_STOP2      7
#define IPODI2C_STEP_WDATA      8       // the data bytes written
#define IPODI2C_STEP_WSTOP      9       // the stop after a write

// DeviceIoControl on "I2C1:" with this code returns an IPODI2C_STATE
#define IOCTL_IPODI2C_GET_STATE \
    CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 3100, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;        // IPODI2C_VERSION
    DWORD   mapped;
    DWORD   bootPcon3;      // bus pins at start-up: bits 4..11 are 0x22 when set up
    DWORD   bootPwrcon1;    // bit 4 = the bus controller's clock gate (gated between transfers)
    DWORD   lastStep;       // IPODI2C_STEP_..., 0 = the last transfer went well
    DWORD   lastCode;       // IPODI2C_E_...
    DWORD   lastStat;       // IICSTAT at that moment
    DWORD   lastSta2;       // IICSTA2 at that moment
    DWORD   reads;          // reads that went well
    DWORD   writes;         // writes that went well
    DWORD   errors;         // transfers that failed on the bus
    DWORD   refused;        // transfers refused before the bus (chip or register not allowed)
    DWORD   lastChip;       // chip and register of the last failed or refused transfer
    DWORD   lastReg;
} IPODI2C_STATE;

#ifdef __cplusplus
}
#endif

#endif
