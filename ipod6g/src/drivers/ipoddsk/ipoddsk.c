//
// ipoddsk.c - the iPod's disk as a Windows CE block device
//
// Stream driver "DSK1:", kernel mode, for the storage manager: it answers
// the disk requests of diskio.h on top of ipodata.c, the hardware layer
// (DMA, about 6.7 MB/s through the file system).
//
// Writing is OFF unless the driver's registry key says "AllowWrite"=1:
//   - off: opening DSK1: for writing fails with ERROR_ACCESS_DENIED, so the
//     storage manager opens the store read only and marks it so (its
//     storedisk.cpp); creating a file fails with error 5, access denied
//     (seen 2026-11-26; the store info does not show it read only); every write
//     request is refused here before the hardware layer is called, and the
//     device info says read only;
//   - on: write requests go to IpodAta_Write. The disk keeps its own write
//     cache on; FLUSH CACHE is sent on Windows' flush request
//     (IOCTL_DISK_FLUSH_CACHE, e.g. restart.exe before a reset) and by a
//     thread FLUSH_DELAY_MS after the last write.
// Format, delete and wipe requests are always refused.
//
// The sector size. The disk answers in blocks of 512 bytes, but an iPod
// that iTunes formatted has a partition table and a FAT32 file system that
// count in 4096 bytes. So at start-up the driver looks where the FAT32 file
// system really is (dskparse.h, as Rockbox does) and shows Windows a disk of
// sectors that big: then the partition table and the file system agree with
// the disk they sit on.
//
// Volume only. When the FAT32 partition is found, Windows is shown that
// partition alone: sector 0 of DSK1: is its boot sector, and the disk's
// partition table (and Apple's firmware partition) stay out of sight. The
// storage manager then takes the store as a disk without partitions and
// mounts the FAT32 file system on all of it. Microsoft's partition driver
// (mspart) refuses the iPod's own table: it lists the FAT32 partition
// (0C, start 49278) before Apple's firmware partition (3F, start 63), and
// mspart rejects any table whose start sectors go down (store
// "unformatted", 0 partitions; 2026-11-26, 80 GB iPod).
//
#include <windows.h>
#include <winioctl.h>
#include <ceddk.h>
#include <pkfuncs.h>
#include <devload.h>
#include <nkintr.h>
#include <diskio.h>
#include <storemgr.h>

#include <bsp_cfg.h>
#include <ipodata.h>
#include <dskparse.h>
#include <ipoddsk.h>

static CRITICAL_SECTION g_cs;           // one command to the disk at a time; also guards g_bounce
static IPODDSK_STATUS   g_ds;
static DISK_INFO        g_info;
static DWORD            g_shift;        // log2 of g_ds.factor
static ULONGLONG        g_base;         // disk block (512 bytes) of Windows' sector 0
static BYTE             g_bounce[IPODATA_MAX_SECTORS * IPODATA_SECTOR_BYTES];

#define FLUSH_DELAY_MS  2000            // the disk's cache is flushed this long after the last write
static BOOL             g_allowWrite;   // registry AllowWrite, read once at load
static HANDLE           g_hWritten;     // set after each write: wakes the flush thread
static volatile DWORD   g_lastWriteTick;

// a read or write request as it is used here: copied out of the caller's memory once
typedef struct {
    DWORD   start;
    DWORD   count;
    DWORD   numSg;
    SG_BUF  sg[MAX_SG_BUF];
} XFER_REQ;

//------------------------------------------------------------------------------
// start-up: who is the disk, and in what sector size does its table count
//
static BOOL ReadOne(DWORD lbaLow, DWORD lbaHigh, BYTE *pSector, void *pContext)
{
    UNREFERENCED_PARAMETER(pContext);
    return IpodAta_Read(lbaLow, lbaHigh, 1, pSector) == IPODATA_OK;
}

static DWORD RegDword(HKEY hKey, LPCWSTR pszName, DWORD dwDefault)
{
    DWORD v, type, size = sizeof(v);

    if (hKey != NULL
        && RegQueryValueEx(hKey, pszName, NULL, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        return v;
    return dwDefault;
}

static void Setup(BOOL useDma)
{
    static BYTE sector[IPODATA_SECTOR_BYTES];
    IPODATA_STATUS st;
    DSK_MBR mbr;
    DSK_FATBOOT boot;
    DWORD tried, factor = 1, low, high;
    int pick, i;

    g_ds.ready = 0;
    g_ds.factor = 1;
    g_shift = 0;
    g_base = 0;

    if (IpodAta_Attach(useDma) != IPODATA_OK)
    {
        g_ds.setupStep = IPODDSK_SETUP_IDENTIFY;
        return;
    }
    IpodAta_GetStatus(&st);
    if (st.sectorBytes != IPODATA_SECTOR_BYTES)
    {
        g_ds.setupStep = IPODDSK_SETUP_SECTOR;
        return;
    }
    if (IpodAta_Read(0, 0, 1, sector) != IPODATA_OK)
    {
        g_ds.setupStep = IPODDSK_SETUP_TABLE;
        return;
    }

    Dsk_ParseMbr(sector, &mbr);
    RETAILMSG(1, (L"ipoddsk: sector 0: signature %u, FAT boot sector %u\r\n",
                  mbr.signature, mbr.looksLikeFat));
    for (i = 0; i < 4; i++)
    {
        g_ds.mbrType[i] = mbr.part[i].type;
        g_ds.mbrStart[i] = mbr.part[i].start;
        g_ds.mbrSize[i] = mbr.part[i].size;
        RETAILMSG(1, (L"ipoddsk: partition %d: type %02X, start %u, size %u\r\n",
                      i, mbr.part[i].type, mbr.part[i].start, mbr.part[i].size));
    }
    pick = Dsk_PickFat(&mbr);
    if (pick >= 0)
    {
        factor = Dsk_FindFat(&mbr.part[pick], ReadOne, NULL, sector, &boot, &tried);
        if (factor)
        {
            g_ds.fatFound = 1;
            g_ds.partStart = mbr.part[pick].start;
            g_ds.partSize = mbr.part[pick].size;
        }
        else
        {
            factor = 1;         // no file system found: show the disk as it is
        }
    }
    g_ds.factor = factor;
    g_shift = (factor == 8) ? 3 : (factor == 4) ? 2 : (factor == 2) ? 1 : 0;

    // the size in sectors of that size; more than 32 bits' worth is cut off
    low = st.sectorsLow;
    high = st.sectorsHigh;
    if (g_shift)
    {
        low = (low >> g_shift) | (high << (32 - g_shift));
        high >>= g_shift;
    }
    if (high)
        low = 0xFFFFFFFF;

    // volume only: the FAT32 partition, cut at the end of the disk
    if (g_ds.fatFound && g_ds.partStart < low)
    {
        if (g_ds.partSize > low - g_ds.partStart)
            g_ds.partSize = low - g_ds.partStart;
        g_base = (ULONGLONG)g_ds.partStart << g_shift;
        low = g_ds.partSize;
        g_ds.volumeOnly = 1;
    }

    memset(&g_info, 0, sizeof(g_info));
    g_info.di_total_sectors = low;
    g_info.di_bytes_per_sect = IPODATA_SECTOR_BYTES << g_shift;
    g_info.di_heads = 255;
    g_info.di_sectors = 63;
    g_info.di_cylinders = low / (255 * 63);
    g_info.di_flags = DISK_INFO_FLAG_CHS_UNCERTAIN;
    if (!g_ds.volumeOnly)
        g_info.di_flags |= DISK_INFO_FLAG_MBR;

    g_ds.bytesPerSector = g_info.di_bytes_per_sect;
    g_ds.totalSectors = low;
    g_ds.setupStep = IPODDSK_SETUP_OK;
    g_ds.ready = 1;
    RETAILMSG(1, (L"ipoddsk: %u sectors of %u bytes (the disk's blocks times %u), FAT32 %s, %s\r\n",
                  low, g_info.di_bytes_per_sect, factor, g_ds.fatFound ? L"found" : L"not found",
                  g_ds.volumeOnly ? L"volume only" : L"whole disk"));
    if (g_ds.volumeOnly)
        RETAILMSG(1, (L"ipoddsk: volume at disk block %u (partition %d, start %u x %u)\r\n",
                      (DWORD)g_base, pick, g_ds.partStart, factor));
}

//------------------------------------------------------------------------------
// one read or write request
//
// Copies the request out of the caller's memory and checks it. Returns a
// Win32 error code. The range check keeps every transfer inside what
// Windows is shown (the FAT32 volume).
static DWORD TakeRequest(const BYTE *pIn, DWORD lenIn, XFER_REQ *pReq)
{
    const SG_REQ *pSg = (const SG_REQ *)pIn;
    DWORD i, bytes = 0, need;

    if (pIn == NULL || lenIn < sizeof(SG_REQ))
        return ERROR_INVALID_PARAMETER;
    __try
    {
        pReq->start = pSg->sr_start;
        pReq->count = pSg->sr_num_sec;
        pReq->numSg = pSg->sr_num_sg;
        if (pReq->numSg == 0 || pReq->numSg > MAX_SG_BUF
            || lenIn < sizeof(SG_REQ) + (pReq->numSg - 1) * sizeof(SG_BUF))
            return ERROR_INVALID_PARAMETER;
        for (i = 0; i < pReq->numSg; i++)
            pReq->sg[i] = pSg->sr_sglist[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return ERROR_INVALID_PARAMETER;
    }

    if (pReq->count == 0)
        return ERROR_INVALID_PARAMETER;
    if (pReq->start >= g_info.di_total_sectors || pReq->count > g_info.di_total_sectors - pReq->start)
        return ERROR_SECTOR_NOT_FOUND;
    if (pReq->count > 0xFFFFFFFF / g_info.di_bytes_per_sect)
        return ERROR_INVALID_PARAMETER;
    need = pReq->count * g_info.di_bytes_per_sect;
    for (i = 0; i < pReq->numSg; i++)
    {
        if (pReq->sg[i].sb_buf == NULL || pReq->sg[i].sb_len == 0 || pReq->sg[i].sb_len > need - bytes)
            return ERROR_INVALID_PARAMETER;
        bytes += pReq->sg[i].sb_len;
    }
    if (bytes != need)
        return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

static void PutStatus(BYTE *pIn, DWORD status)
{
    __try
    {
        ((SG_REQ *)pIn)->sr_status = status;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// A read (write FALSE) or write request. The caller has checked that
// writing is allowed.
static DWORD DoTransfer(BYTE *pIn, DWORD lenIn, BOOL write)
{
    XFER_REQ req;
    PUCHAR mapped[MAX_SG_BUF];
    DWORD status, i, opened = 0, t0, ms, argType = write ? ARG_I_PTR : ARG_O_PTR;
    DWORD lbaLow, lbaHigh, left, n, got, sgIndex = 0, sgOff = 0;
    ULONGLONG lba;

    if (!g_ds.ready)
        return ERROR_NOT_READY;

    status = TakeRequest(pIn, lenIn, &req);
    if (status != ERROR_SUCCESS)
    {
        g_ds.badRequests++;
        if (pIn != NULL && lenIn >= sizeof(SG_REQ))
            PutStatus(pIn, status);
        return status;
    }

    // the caller's buffers, made reachable from here
    for (opened = 0; opened < req.numSg; opened++)
    {
        if (FAILED(CeOpenCallerBuffer((PVOID *)&mapped[opened], req.sg[opened].sb_buf,
                                      req.sg[opened].sb_len, argType, FALSE)))
        {
            status = ERROR_INVALID_PARAMETER;
            g_ds.badRequests++;
            break;
        }
    }

    if (status == ERROR_SUCCESS)
    {
        t0 = GetTickCount();
        // in the disk's own 512-byte blocks, from the volume's start; may pass 32 bits
        lba = g_base + ((ULONGLONG)req.start << g_shift);
        lbaLow = (DWORD)lba;
        lbaHigh = (DWORD)(lba >> 32);
        left = req.count << g_shift;

        while (left && status == ERROR_SUCCESS)
        {
            n = (left > IPODATA_MAX_SECTORS) ? IPODATA_MAX_SECTORS : left;

            // one command to the disk, and its data handed over, per turn of the lock
            EnterCriticalSection(&g_cs);
            if (!write && IpodAta_Read(lbaLow, lbaHigh, n, g_bounce) != IPODATA_OK)
                status = ERROR_READ_FAULT;
            if (status == ERROR_SUCCESS)
            {
                // between the caller's buffers and g_bounce, in the direction of the transfer
                __try
                {
                    got = 0;
                    while (got < n * IPODATA_SECTOR_BYTES)
                    {
                        DWORD part = req.sg[sgIndex].sb_len - sgOff;

                        if (part > n * IPODATA_SECTOR_BYTES - got)
                            part = n * IPODATA_SECTOR_BYTES - got;
                        if (write)
                            memcpy(g_bounce + got, mapped[sgIndex] + sgOff, part);
                        else
                            memcpy(mapped[sgIndex] + sgOff, g_bounce + got, part);
                        got += part;
                        sgOff += part;
                        if (sgOff == req.sg[sgIndex].sb_len)
                        {
                            sgIndex++;
                            sgOff = 0;
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    status = ERROR_INVALID_PARAMETER;
                }
            }
            if (write && status == ERROR_SUCCESS)
            {
                // before the write: a flush started now still covers it, as
                // the flush thread waits for the lock this write holds
                g_ds.flushPending = 1;
                g_lastWriteTick = GetTickCount();
                if (IpodAta_Write(lbaLow, lbaHigh, n, g_bounce) != IPODATA_OK)
                    status = ERROR_WRITE_FAULT;
            }
            LeaveCriticalSection(&g_cs);

            lbaLow += n;
            if (lbaLow < n)
                lbaHigh++;
            left -= n;
        }

        ms = GetTickCount() - t0;
        if (write)
        {
            if (status == ERROR_SUCCESS)
            {
                g_ds.writes++;
                g_ds.sectorsWritten += req.count;
                g_ds.writeMs += ms;
            }
            else if (status == ERROR_WRITE_FAULT)
            {
                g_ds.writeErrors++;
            }
            if (g_hWritten != NULL)
                SetEvent(g_hWritten);       // the flush thread: flush in FLUSH_DELAY_MS
        }
        else if (status == ERROR_SUCCESS)
        {
            g_ds.reads++;
            g_ds.sectorsRead += req.count;
            g_ds.readMs += ms;
            if (ms > g_ds.maxReadMs)
                g_ds.maxReadMs = ms;
            g_ds.lastStart = req.start;
            g_ds.lastCount = req.count;
        }
        else if (status == ERROR_READ_FAULT)
        {
            g_ds.readErrors++;
        }
    }

    for (i = 0; i < opened; i++)
        CeCloseCallerBuffer(mapped[i], req.sg[i].sb_buf, req.sg[i].sb_len, argType);

    PutStatus(pIn, status);
    return status;
}

//------------------------------------------------------------------------------
// the disk's write cache
//
// Flushes it if anything was written since the last flush. Returns a Win32
// error code.
static DWORD DoFlush(void)
{
    DWORD status = ERROR_SUCCESS;

    EnterCriticalSection(&g_cs);
    if (g_ds.flushPending)
    {
        g_ds.flushPending = 0;
        g_ds.flushesDone++;
        if (IpodAta_Flush() != IPODATA_OK)
        {
            g_ds.flushPending = 1;          // try again next time
            g_ds.flushErrors++;
            status = ERROR_WRITE_FAULT;
        }
    }
    LeaveCriticalSection(&g_cs);
    return status;
}

// Flushes FLUSH_DELAY_MS after the last write, so the cache is not left
// full while nothing else happens.
static DWORD WINAPI FlushThread(LPVOID pArg)
{
    DWORD idle;

    UNREFERENCED_PARAMETER(pArg);
    for (;;)
    {
        WaitForSingleObject(g_hWritten, INFINITE);
        for (;;)
        {
            idle = GetTickCount() - g_lastWriteTick;
            if (idle >= FLUSH_DELAY_MS)
                break;
            Sleep(FLUSH_DELAY_MS - idle);
        }
        DoFlush();
    }
}

//------------------------------------------------------------------------------
static BOOL CopyOut(void *pDest, DWORD destLen, const void *pSrc, DWORD len, PDWORD pdwActualOut)
{
    if (pDest == NULL || destLen < len)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    __try
    {
        memcpy(pDest, pSrc, len);
        if (pdwActualOut != NULL)
            *pdwActualOut = len;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

static BOOL Refuse(DWORD *pCounter, DWORD error)
{
    (*pCounter)++;
    SetLastError(error);
    return FALSE;
}

//------------------------------------------------------------------------------
// Stream interface
//
DWORD DSK_Init(LPCTSTR pContext, DWORD dwBusContext)
{
    HKEY hKey;
    DWORD useDma, sysIntr, allowWrite;
    IPODATA_STATUS st;
    HANDLE hThread;

    UNREFERENCED_PARAMETER(dwBusContext);

    // the driver's own registry key: "UseDma" 0 = PIO only (default 1);
    // "SysIntr" for DMA completion (default SYSINTR_ATA), 0 = polled;
    // "AllowWrite" 1 = writes reach the disk (default 0: read only)
    hKey = OpenDeviceKey(pContext);
    useDma = RegDword(hKey, L"UseDma", 1);
    sysIntr = RegDword(hKey, L"SysIntr", SYSINTR_ATA);
    allowWrite = RegDword(hKey, L"AllowWrite", 0);
    if (hKey != NULL)
        RegCloseKey(hKey);

    InitializeCriticalSection(&g_cs);
    memset(&g_ds, 0, sizeof(g_ds));
    g_ds.version = IPODDSK_VERSION;
    IpodAta_Init();
    Setup(useDma != 0);

    // the interrupt only once DMA is in use (the reads above were polled)
    IpodAta_GetStatus(&st);
    if (st.dmaMode != IPODATA_MODE_PIO && sysIntr != 0)
        IpodAta_UseInterrupt(sysIntr);

    // writing: only with the flush thread running, so the cache is not left unflushed
    g_allowWrite = FALSE;
    if (allowWrite == 1 && g_ds.ready)
    {
        g_hWritten = CreateEvent(NULL, FALSE, FALSE, NULL);
        hThread = (g_hWritten != NULL) ? CreateThread(NULL, 0, FlushThread, NULL, 0, NULL) : NULL;
        if (hThread != NULL)
        {
            CloseHandle(hThread);
            g_allowWrite = TRUE;
        }
    }
    g_ds.allowWrite = g_allowWrite ? 1 : 0;
    RETAILMSG(1, (L"ipoddsk: writing %s (registry AllowWrite %u)\r\n",
                  g_allowWrite ? L"ALLOWED" : L"off, read only", allowWrite));
    return 1;                   // stay loaded in any case, so the panel can say what is wrong
}

BOOL DSK_Deinit(DWORD hDeviceContext)
{
    UNREFERENCED_PARAMETER(hDeviceContext);
    return TRUE;
}

DWORD DSK_Open(DWORD hDeviceContext, DWORD AccessCode, DWORD ShareMode)
{
    UNREFERENCED_PARAMETER(ShareMode);

    // The storage manager opens the store for reading and writing first; on
    // ERROR_ACCESS_DENIED it opens it for reading only and marks the store
    // read only (fsdmgr storedisk.cpp). The device manager passes the code on.
    if (!g_allowWrite && (AccessCode & GENERIC_WRITE))
    {
        g_ds.opensRefused++;
        SetLastError(ERROR_ACCESS_DENIED);
        return 0;
    }
    return hDeviceContext;
}

BOOL DSK_Close(DWORD hOpenContext)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    return TRUE;
}

DWORD DSK_Read(DWORD hOpenContext, LPVOID pBuffer, DWORD Count)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(Count);
    return (DWORD)-1;
}

DWORD DSK_Write(DWORD hOpenContext, LPCVOID pBuffer, DWORD Count)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(pBuffer);
    UNREFERENCED_PARAMETER(Count);
    SetLastError(ERROR_WRITE_PROTECT);
    return (DWORD)-1;
}

DWORD DSK_Seek(DWORD hOpenContext, long Amount, WORD Type)
{
    UNREFERENCED_PARAMETER(hOpenContext);
    UNREFERENCED_PARAMETER(Amount);
    UNREFERENCED_PARAMETER(Type);
    return (DWORD)-1;
}

BOOL DSK_IOControl(DWORD hOpenContext, DWORD dwCode, PBYTE pBufIn, DWORD dwLenIn,
                   PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    STORAGEDEVICEINFO sdi;
    IPODDSK_STATUS st;
    DWORD status;

    UNREFERENCED_PARAMETER(hOpenContext);

    switch (dwCode)
    {
    case IOCTL_DISK_READ:
    case DISK_IOCTL_READ:
        status = DoTransfer(pBufIn, dwLenIn, FALSE);
        SetLastError(status);
        return status == ERROR_SUCCESS;

    case IOCTL_DISK_WRITE:
    case DISK_IOCTL_WRITE:
        if (!g_allowWrite)
        {
            if (pBufIn != NULL && dwLenIn >= sizeof(SG_REQ))
                PutStatus(pBufIn, ERROR_WRITE_PROTECT);
            return Refuse(&g_ds.writesRefused, ERROR_WRITE_PROTECT);
        }
        status = DoTransfer(pBufIn, dwLenIn, TRUE);
        SetLastError(status);
        return status == ERROR_SUCCESS;

    // Nothing below this line and above the next one touches the disk.
    case IOCTL_DISK_FORMAT_MEDIA:
    case DISK_IOCTL_FORMAT_MEDIA:
    case IOCTL_DISK_DELETE_SECTORS:
    case IOCTL_DISK_DELETE_CLUSTER:
    case IOCTL_DISK_SECURE_WIPE:
    case IOCTL_DISK_SET_SECURE_WIPE_FLAG:
    case IOCTL_DISK_SECURE_DELETE:
    case IOCTL_DISK_FORMAT_VOLUME:
    case IOCTL_DISK_WRITE_CDROM:
        return Refuse(&g_ds.formatsRefused, ERROR_WRITE_PROTECT);
    // ---------------------------------------------------------------------

    case IOCTL_DISK_GETINFO:
        g_ds.getInfos++;
        if (!g_ds.ready)
        {
            SetLastError(ERROR_NOT_READY);
            return FALSE;
        }
        return CopyOut(pBufOut, dwLenOut, &g_info, sizeof(g_info), pdwActualOut);

    case DISK_IOCTL_GETINFO:            // the old code answers through its input buffer
        g_ds.getInfos++;
        if (!g_ds.ready)
        {
            SetLastError(ERROR_NOT_READY);
            return FALSE;
        }
        return CopyOut(pBufIn, dwLenIn, &g_info, sizeof(g_info), pdwActualOut);

    case IOCTL_DISK_SETINFO:
    case DISK_IOCTL_SETINFO:            // nothing is taken over: the disk is what it is
        g_ds.setInfos++;
        return TRUE;

    case IOCTL_DISK_INITIALIZED:
    case DISK_IOCTL_INITIALIZED:
        g_ds.initializeds++;
        return TRUE;

    case IOCTL_DISK_GETNAME:
    case DISK_IOCTL_GETNAME:            // no name of our own: the profile's folder name is used
        return Refuse(&g_ds.getNames, ERROR_NOT_SUPPORTED);

    case IOCTL_DISK_DEVICE_INFO:
        g_ds.deviceInfos++;
        memset(&sdi, 0, sizeof(sdi));
        sdi.cbSize = sizeof(sdi);
        wcscpy(sdi.szProfile, IPODDSK_PROFILE);
        sdi.dwDeviceClass = STORAGE_DEVICE_CLASS_BLOCK;
        sdi.dwDeviceType = STORAGE_DEVICE_TYPE_ATA;
        sdi.dwDeviceFlags = g_allowWrite ? STORAGE_DEVICE_FLAG_READWRITE : STORAGE_DEVICE_FLAG_READONLY;
        // asked through the input buffer; through the output buffer by some
        if (pBufIn != NULL && dwLenIn == sizeof(sdi))
            return CopyOut(pBufIn, dwLenIn, &sdi, sizeof(sdi), pdwActualOut);
        return CopyOut(pBufOut, dwLenOut, &sdi, sizeof(sdi), pdwActualOut);

    case IOCTL_DISK_FLUSH_CACHE:        // the disk's cache, if anything was written
        g_ds.flushes++;
        status = DoFlush();
        SetLastError(status);
        return status == ERROR_SUCCESS;

    case IOCTL_IPODDSK_GET_STATUS:
        EnterCriticalSection(&g_cs);
        IpodAta_GetStatus(&g_ds.ata);
        st = g_ds;
        LeaveCriticalSection(&g_cs);
        return CopyOut(pBufOut, dwLenOut, &st, sizeof(st), pdwActualOut);
    }

    g_ds.others++;
    g_ds.lastOther = dwCode;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

BOOL WINAPI DllMain(HANDLE hInstance, DWORD dwReason, LPVOID pReserved)
{
    UNREFERENCED_PARAMETER(pReserved);
    if (dwReason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls((HMODULE)hInstance);
    return TRUE;
}
