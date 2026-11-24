//
// ipodi2c.h - reading the iPod's power chip under Windows CE: the bus layer
// (ipodi2c.c), the state it keeps and the battery measurement's scale
//
#ifndef IPODI2C_H
#define IPODI2C_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODI2C_VERSION         1
#define IPODI2C_PMU             0xE6    // the power chip's address on bus 0 (write form)
#define IPODI2C_MAX_READ        16

// error codes (lastCode); lastStep says where
#define IPODI2C_OK              0
#define IPODI2C_E_NOT_MAPPED    1       // no registers
#define IPODI2C_E_PINS          2       // the bus pins are not set up as Rockbox sets them
#define IPODI2C_E_NOT_READY     3       // the controller never became ready for a register access
#define IPODI2C_E_NO_END        4       // a byte never finished on the bus
#define IPODI2C_E_NO_ANSWER     5       // the chip did not acknowledge
#define IPODI2C_E_PARAM         6

// steps (lastStep)
#define IPODI2C_STEP_CHECK      1
#define IPODI2C_STEP_ADDRESS    2       // start + chip address, to send the register number
#define IPODI2C_STEP_REGISTER   3       // the register number
#define IPODI2C_STEP_STOP1      4
#define IPODI2C_STEP_READ       5       // start + chip address, to read
#define IPODI2C_STEP_DATA       6       // the data bytes
#define IPODI2C_STEP_STOP2      7
#define IPODI2C_STEP_MEASURE1   8       // battery measurement: its first setting
#define IPODI2C_STEP_MEASURE2   9       // battery measurement: its second setting and the start

// Maps the registers and notes what Rockbox left. Writes nothing.
BOOL  IpodI2c_Init(void);

// Reads count registers of a chip, starting at firstReg. On the bus this
// sends the chip's address and the register number, nothing else. 0 = ok.
DWORD IpodI2c_Read(BYTE chip, BYTE firstReg, BYTE *pOut, DWORD count);

// The one thing this layer can make a chip do: start a battery voltage
// measurement in the power chip, with the three bytes Rockbox sends for it.
// The bytes are fixed in ipodi2c.c;
// nothing can be passed in. The result is read with IpodI2c_Read. 0 = ok.
DWORD IpodI2c_StartBatteryMeasurement(void);

// the account of the last operation and the counters
typedef struct {
    DWORD   mapped;
    DWORD   bootPcon3;      // bus pins: bits 4..11 are 0x22 when set up
    DWORD   bootPwrcon1;    // bit 4 = the bus controller's clock gate (Rockbox gates it between transfers)
    DWORD   lastStep;       // IPODI2C_STEP_..., 0 = it went well
    DWORD   lastCode;       // IPODI2C_E_...
    DWORD   lastStat;       // IICSTAT at that moment
    DWORD   lastSta2;       // IICSTA2 at that moment
    DWORD   reads;          // read calls that went well
    DWORD   measurements;   // battery measurements started
    DWORD   errors;
} IPODI2C_STATE;
void  IpodI2c_GetState(IPODI2C_STATE *pOut);

//------------------------------------------------------------------------------
// the battery measurement in Rockbox's terms: raw is 10 bits,
// millivolts = raw * 2000 / 1023 + 2250
#define PMUTEST_MV(raw)     ((raw) * 2000 / 1023 + 2250)

#ifdef __cplusplus
}
#endif

#endif
