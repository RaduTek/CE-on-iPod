//
// ipoddsk.h - the iPod's disk as a Windows CE block device (ipoddsk.dll,
// "DSK1:"): what its panel can ask
//
#ifndef IPODDSK_H
#define IPODDSK_H

#include <winioctl.h>
#include <ipodata.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODDSK_VERSION     2
#define IPODDSK_PROFILE     L"iPodDisk"     // System\StorageManager\Profiles\iPodDisk

// where the look at the disk at start-up stopped (IPODDSK_STATUS.setupStep)
#define IPODDSK_SETUP_OK        0
#define IPODDSK_SETUP_IDENTIFY  1       // the disk did not identify itself (see ata.lastStep ...)
#define IPODDSK_SETUP_SECTOR    2       // its sectors are not 512 bytes
#define IPODDSK_SETUP_TABLE     3       // sector 0 could not be read

#define IOCTL_IPODDSK_GET_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x940, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;
    DWORD   ready;          // the disk answered: Windows may read it
    DWORD   setupStep;      // IPODDSK_SETUP_...
    DWORD   fatFound;       // a FAT32 file system was found at start-up
    DWORD   factor;         // 512-byte blocks of the disk per sector shown to Windows
    DWORD   bytesPerSector; // as shown to Windows
    DWORD   totalSectors;   // as shown to Windows
    DWORD   partStart;      // the FAT32 partition, in those sectors
    DWORD   partSize;
    DWORD   volumeOnly;     // 1: Windows sees only that partition (sector 0 = its boot sector)
    BYTE    mbrType[4];     // sector 0's partition table as found at start-up
    DWORD   mbrStart[4];    // (in the table's own units)
    DWORD   mbrSize[4];

    // what Windows asked for
    DWORD   reads;          // read requests carried out
    DWORD   sectorsRead;    // in sectors as shown to Windows
    DWORD   readMs;         // time spent in them
    DWORD   maxReadMs;
    DWORD   lastStart;
    DWORD   lastCount;
    DWORD   readErrors;     // read requests the disk failed (see ata.lastStep ...)
    DWORD   badRequests;    // read requests refused for their parameters
    DWORD   writesRefused;  // write requests: all refused, none reaches the disk
    DWORD   formatsRefused; // format, delete and wipe requests: likewise
    DWORD   getInfos;
    DWORD   deviceInfos;
    DWORD   getNames;
    DWORD   setInfos;
    DWORD   initializeds;
    DWORD   flushes;
    DWORD   others;         // requests this driver does not know
    DWORD   lastOther;      // the code of the last of those

    IPODATA_STATUS ata;     // the hardware layer's own account
} IPODDSK_STATUS;

#ifdef __cplusplus
}
#endif

#endif
