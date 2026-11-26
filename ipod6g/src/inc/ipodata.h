//
// ipodata.h - reading and writing the iPod's disk under Windows CE: the hardware layer
// (ipodata.c) and the status it keeps (the iPod disk panel shows it)
//
#ifndef IPODATA_H
#define IPODATA_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODATA_VERSION         6
#define IPODATA_SECTOR_BYTES    512
#define IPODATA_MAX_SECTORS     128     // per read call (64 KB, the DMA buffer)

// transfer modes (IPODATA_STATUS.dmaMode), as SET FEATURES 0x03 takes them
#define IPODATA_MODE_PIO        0
#define IPODATA_MODE_MWDMA(n)   (0x20 | (n))
#define IPODATA_MODE_UDMA(n)    (0x40 | (n))

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
#define IPODATA_E_DMA_TIMEOUT   16      // the controller never said the DMA transfer was done
#define IPODATA_E_SUPPLY        17      // the power chip did not take the disk's supply setting
#define IPODATA_E_CTRL_STOP     18      // the controller did not report itself stopped
#define IPODATA_E_OTHER_DISK    19      // after power-up the disk reports another size

// steps (IPODATA_STATUS.lastStep)
#define IPODATA_STEP_CHECK      1       // looking at what Rockbox left
#define IPODATA_STEP_ID_IDLE    2       // identify: waiting for the disk to be idle
#define IPODATA_STEP_ID_CMD     3       // identify: sending the command
#define IPODATA_STEP_ID_DATA    4       // identify: waiting for / taking the data
#define IPODATA_STEP_RD_READY   5       // read: waiting for the disk to be ready
#define IPODATA_STEP_RD_CMD     6       // read: sending the command
#define IPODATA_STEP_RD_DATA    7       // read: waiting for / taking the data
#define IPODATA_STEP_RD_END     8       // read: after the last sector
#define IPODATA_STEP_RD_DMA     10      // read: the DMA transfer
#define IPODATA_STEP_WR_READY   11      // write: waiting for the disk to be ready
#define IPODATA_STEP_WR_CMD     12      // write: sending the command
#define IPODATA_STEP_WR_DATA    13      // write: waiting for / giving the data (PIO)
#define IPODATA_STEP_WR_DMA     14      // write: the DMA transfer
#define IPODATA_STEP_WR_END     15      // write: after the last sector
#define IPODATA_STEP_FLUSH      16      // flush cache
#define IPODATA_STEP_STANDBY    17      // standby immediate (spin down)
#define IPODATA_STEP_POWER_OFF  18      // switching the disk off
#define IPODATA_STEP_POWER_ON   19      // power, pins, controller reset
#define IPODATA_STEP_PU_ID      20      // power-up: the disk that answered is not the one shown

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
    DWORD   bootMdmaTime;
    DWORD   bootUdmaTime;

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
    DWORD   word53;         // which of words 64-70 and 88 are valid
    DWORD   word63;         // multiword DMA modes
    DWORD   word93;         // cable (bit 13: 80-wire)

    // the transfer mode
    DWORD   dmaWanted;      // the caller asked for DMA (registry UseDma)
    DWORD   dmaMode;        // IPODATA_MODE_...: what reads use
    DWORD   dmaBufPhys;     // physical address of the DMA buffer, 0 = none
    DWORD   modeError;      // SET FEATURES failed: IPODATA_E_... (then PIO)

    // counters
    DWORD   identifies;
    DWORD   reads;
    DWORD   sectorsRead;
    DWORD   errors;
    DWORD   lastReadMs;     // duration of the last read call
    DWORD   lastReadSectors;
    DWORD   dmaReads;       // reads done by DMA
    DWORD   dmaTimeouts;

    // DMA completion by interrupt (IpodAta_UseInterrupt); 0 = polled
    DWORD   sysIntr;        // the SYSINTR in use, 0 = none
    DWORD   intrError;      // InterruptInitialize failed: GetLastError()
    DWORD   intrWakes;      // DMA reads ended by the interrupt
    DWORD   intrMissed;     // ... where the transfer was done but no interrupt came in time
    DWORD   intrSpurious;   // the event fired with the transfer not done

    // writes and flushes
    DWORD   writes;         // write calls
    DWORD   sectorsWritten;
    DWORD   dmaWrites;      // ... done by DMA
    DWORD   writeRetries;   // DMA writes that failed and were done again in PIO
    DWORD   lastWriteMs;
    DWORD   flushCmd;       // the disk's flush command (E7/EA), 0 = it has none
    DWORD   flushes;        // flush commands sent
    DWORD   flushErrors;
    DWORD   lastFlushMs;
    DWORD   word80;         // ATA versions
    DWORD   word82;         // features supported (bit 5: write cache)
    DWORD   word85;         // features enabled (bit 5: write cache on)

    // spinning down (IpodAta_Standby); the next command spins the disk up
    DWORD   standby;        // 1: told to spin down, no command since
    DWORD   spinDowns;
    DWORD   spinDownErrors;
    DWORD   spinUps;        // transfers that started with the disk in standby
    DWORD   lastSpinUpMs;   // how long the first transfer after standby took
    DWORD   maxSpinUpMs;

    // switching the disk off (IpodAta_PowerOff); the next command powers it up
    DWORD   canPowerManage; // identify words 82/85 bit 3: the disk takes STANDBY IMMEDIATE
    DWORD   poweredOff;     // 1: supply, pins and controller off
    DWORD   powerOffs;
    DWORD   powerOffErrors;
    DWORD   powerUps;
    DWORD   powerUpErrors;
    DWORD   lastPowerUpMs;  // supply on to the disk answering IDENTIFY with its mode set
    DWORD   maxPowerUpMs;
    DWORD   featureErrors;  // SET FEATURES at power-up the disk refused (not fatal)

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
// (two register writes, no reset) and asks the disk who it is. With useDma,
// then picks the fastest DMA mode the disk offers, sets the controller's
// timing for it and tells the disk (SET FEATURES); if any of that fails,
// reads stay PIO. 0 = ok (identify worked).
DWORD IpodAta_Attach(BOOL useDma);

// Reads count sectors of 512 bytes. 0 = ok.
DWORD IpodAta_Read(DWORD lbaLow, DWORD lbaHigh, DWORD count, BYTE *pOut);

// Writes count sectors of 512 bytes. 0 = ok. Nothing here decides whether
// writing is allowed: ipoddsk.c does (registry AllowWrite) and calls this
// only then.
DWORD IpodAta_Write(DWORD lbaLow, DWORD lbaHigh, DWORD count, const BYTE *pIn);

// Has the disk write its cache to the platter (FLUSH CACHE / FLUSH CACHE
// EXT). 0 = ok, also when the disk has no such command.
DWORD IpodAta_Flush(void);

// Spins the disk down (STANDBY IMMEDIATE, which also writes back its
// cache). The disk stays powered and the controller as it is; the next read
// or write spins it up again by itself, and that one transfer is given
// extra time for it. 0 = ok, also when it is in standby already.
DWORD IpodAta_Standby(void);

// Switches the disk off, as Rockbox's ata_sleepnow() + ata_power_down():
// flush and standby (or only the flush when the disk has no power
// management), the controller stopped and its clock gated, the disk's pins
// released, and its supply off at the power chip (I2C1:, register 0x1B).
// The next read or write powers it up again: supply, pins, controller
// reset, IDENTIFY, timing and transfer mode, as Rockbox's ata_power_up().
// 0 = ok, also when it is off already.
DWORD IpodAta_PowerOff(void);

// DMA reads wait for the ATA interrupt on sysIntr (SYSINTR_ATA) instead of
// polling. Quiets the controller's interrupt sources first. FALSE = it
// could not be set up: polling stays (intrError says why).
BOOL  IpodAta_UseInterrupt(DWORD sysIntr);

void  IpodAta_GetStatus(IPODATA_STATUS *pOut);

#ifdef __cplusplus
}
#endif

#endif
