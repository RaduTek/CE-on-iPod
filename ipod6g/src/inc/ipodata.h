//
// ipodata.h - reading the iPod's disk under Windows CE: the hardware layer
// (ipodata.c) and the status it keeps (the iPod disk panel shows it)
//
#ifndef IPODATA_H
#define IPODATA_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODATA_VERSION         1
#define IPODATA_SECTOR_BYTES    512
#define IPODATA_MAX_SECTORS     64      // per read call

// error codes (IPODATA_STATUS.lastCode); lastStep says where
#define IPODATA_OK              0
#define IPODATA_E_NOT_MAPPED    1       // no registers
#define IPODATA_E_CLOCK_OFF     2       // the disk controller's clock is gated
#define IPODATA_E_POWERED_DOWN  3       // Rockbox had switched the disk off (pins not set up)
#define IPODATA_E_CEATA         4       // the disk is on the CE-ATA interface: not this driver
#define IPODATA_E_CTRL_OFF      5       // the disk controller is disabled
#define IPODATA_E_HANDSHAKE     6       // the controller never said "ready" for a register access
#define IPODATA_E_BUSY          7       // the disk stayed busy
#define IPODATA_E_NOT_READY     8       // the disk never said "ready"
#define IPODATA_E_NO_DATA       9       // the disk never offered data
#define IPODATA_E_DEVICE        10      // the disk reported an error (see lastError)
#define IPODATA_E_END           11      // wrong state after the last data word
#define IPODATA_E_NOT_ATTACHED  12      // no successful identify yet
#define IPODATA_E_SECTOR_SIZE   13      // sectors are not 512 bytes
#define IPODATA_E_RANGE         14      // outside the disk, or too many sectors at once
#define IPODATA_E_PARAM         15

// steps (IPODATA_STATUS.lastStep)
#define IPODATA_STEP_CHECK      1       // looking at what Rockbox left
#define IPODATA_STEP_ID_IDLE    2       // identify: waiting for the disk to be idle
#define IPODATA_STEP_ID_CMD     3       // identify: sending the command
#define IPODATA_STEP_ID_DATA    4       // identify: waiting for / taking the data
#define IPODATA_STEP_RD_READY   5       // read: waiting for the disk to be ready
#define IPODATA_STEP_RD_CMD     6       // read: sending the command
#define IPODATA_STEP_RD_DATA    7       // read: waiting for / taking the data
#define IPODATA_STEP_RD_END     8       // read: after the last sector

typedef struct {
    DWORD   version;
    DWORD   mapped;

    // as Rockbox left things, read once at load before anything was written
    DWORD   bootPwrcon0;    // bit 5 = the disk controller's clock gate (0 = running)
    DWORD   bootPcon7;      // the disk's pins: 44444444 when set up, 0 when Rockbox switched the disk off
    DWORD   bootPcon8;
    DWORD   bootPcon9;
    DWORD   bootPcon10;     // low 16 bits
    DWORD   bootPdat11;     // bit 1 = CE-ATA
    DWORD   bootControl;
    DWORD   bootCfg;
    DWORD   bootPioTime;
    DWORD   bootIrq;
    DWORD   bootIrqMask;

    // the last operation
    DWORD   attached;       // an identify succeeded
    DWORD   lastStep;       // IPODATA_STEP_..., 0 = it went well
    DWORD   lastCode;       // IPODATA_E_...
    DWORD   lastStatus;     // the disk's status byte at that moment
    DWORD   lastError;      // the disk's error byte, when it reported one

    // from the identify data
    char    model[44];
    char    firmware[12];
    DWORD   lba48;
    DWORD   sectorsLow;
    DWORD   sectorsHigh;
    DWORD   sectorBytes;
    DWORD   word49;         // capabilities
    DWORD   word83;         // command sets
    DWORD   word88;         // UDMA modes
    DWORD   word106;        // sector size info

    // counters
    DWORD   identifies;
    DWORD   reads;
    DWORD   sectorsRead;
    DWORD   errors;
    DWORD   lastReadMs;     // duration of the last read call
    DWORD   lastReadSectors;

    // registers now
    DWORD   control;
    DWORD   cfg;
    DWORD   pioTime;
    DWORD   irq;
    DWORD   irqMask;
} IPODATA_STATUS;

// Maps the registers and notes what Rockbox left. Writes nothing.
BOOL  IpodAta_Init(void);

// Puts the controller in the state Rockbox itself reads the disk's identity in
// (two register writes, no reset) and asks the disk who it is. 0 = ok.
DWORD IpodAta_Attach(void);

// Reads count sectors of 512 bytes. 0 = ok. There is no write function.
DWORD IpodAta_Read(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE *pOut);

void  IpodAta_GetStatus(IPODATA_STATUS *pOut);

#ifdef __cplusplus
}
#endif

#endif
