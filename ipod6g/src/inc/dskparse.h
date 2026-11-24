//
// dskparse.h - what the disk panel reads out of sector 0 (partition table)
// and out of the first sector of a FAT partition. Shared with the PC test.
//
#ifndef DSKPARSE_H
#define DSKPARSE_H

typedef struct {
    BYTE    type;
    DWORD   start;
    DWORD   size;
} DSK_PART;

typedef struct {
    BOOL    signature;      // 55 AA at the end
    BOOL    looksLikeFat;   // sector 0 is itself a FAT boot sector: a disk without partitions
    DSK_PART part[4];
} DSK_MBR;

typedef struct {
    BOOL    signature;      // 55 AA at the end
    BOOL    fat32;          // "FAT32   " where FAT32 writes it
    char    oem[9];
    char    label[12];
    DWORD   bytesPerSector;
    DWORD   sectorsPerCluster;
    DWORD   reservedSectors;
    DWORD   fats;
    DWORD   sectorsPerFat;
    DWORD   totalSectors;
    DWORD   rootCluster;
} DSK_FATBOOT;

static DWORD Dsk_Le16(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8);
}

static DWORD Dsk_Le32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

// n characters, made printable, trailing blanks removed
static void Dsk_Text(char *pOut, const BYTE *p, int n)
{
    int i;

    for (i = 0; i < n; i++)
        pOut[i] = (p[i] >= 0x20 && p[i] <= 0x7E) ? (char)p[i] : '?';
    while (n > 0 && pOut[n - 1] == ' ')
        n--;
    pOut[n] = 0;
}

static void Dsk_ParseFatBoot(const BYTE *s, DSK_FATBOOT *b)
{
    memset(b, 0, sizeof(*b));
    b->signature = (s[510] == 0x55 && s[511] == 0xAA);
    b->fat32 = (memcmp(s + 82, "FAT32   ", 8) == 0);
    Dsk_Text(b->oem, s + 3, 8);
    Dsk_Text(b->label, s + 71, 11);
    b->bytesPerSector = Dsk_Le16(s + 11);
    b->sectorsPerCluster = s[13];
    b->reservedSectors = Dsk_Le16(s + 14);
    b->fats = s[16];
    b->sectorsPerFat = Dsk_Le32(s + 36);
    b->totalSectors = Dsk_Le32(s + 32);
    b->rootCluster = Dsk_Le32(s + 44);
}

static void Dsk_ParseMbr(const BYTE *s, DSK_MBR *m)
{
    int i;

    memset(m, 0, sizeof(*m));
    m->signature = (s[510] == 0x55 && s[511] == 0xAA);
    m->looksLikeFat = (m->signature && (s[0] == 0xEB || s[0] == 0xE9)
                       && memcmp(s + 82, "FAT32   ", 8) == 0);
    for (i = 0; i < 4; i++)
    {
        const BYTE *e = s + 446 + i * 16;

        m->part[i].type = e[4];
        m->part[i].start = Dsk_Le32(e + 8);
        m->part[i].size = Dsk_Le32(e + 12);
    }
}

// The partition to look for a FAT32 file system in: the first one of a FAT32
// type (0B, 0C). An iPod's first partition (type 00, its own firmware) is
// not one. -1 = none.
static int Dsk_PickFat(const DSK_MBR *m)
{
    int i;

    if (!m->signature || m->looksLikeFat)
        return -1;
    for (i = 0; i < 4; i++)
    {
        if (m->part[i].size != 0 && (m->part[i].type == 0x0B || m->part[i].type == 0x0C))
            return i;
    }
    return -1;
}

// One 512-byte block of the disk. FALSE = it could not be read.
typedef BOOL (*DSK_READ_FN)(DWORD lbaLow, DWORD lbaHigh, BYTE *pSector, void *pContext);

// Where a partition's FAT32 file system really starts.
//
// The disk counts in blocks of 512 bytes. The partition table of an iPod
// that iTunes formatted counts in blocks of 4096 bytes (the size of the
// sectors of the disk Apple built in), and so does the FAT32 file system in
// its own "bytes per sector" field. Rockbox handles this by trying the
// partition start times 1, 2, 4 and 8 (firmware/common/disk.c) and so does
// this: the factor is right when a FAT32 boot sector is found there whose
// bytes per sector is 512 times that factor.
//
// Returns the factor (1, 2, 4 or 8) with the boot sector in pSector and its
// fields in pBoot, or 0 when none fits (*pTried = how many places were read).
static DWORD Dsk_FindFat(const DSK_PART *pPart, DSK_READ_FN pfnRead, void *pContext,
                         BYTE *pSector, DSK_FATBOOT *pBoot, DWORD *pTried)
{
    DWORD shift;

    *pTried = 0;
    for (shift = 0; shift <= 3; shift++)
    {
        DWORD low = pPart->start << shift;
        DWORD high = shift ? (pPart->start >> (32 - shift)) : 0;

        if (!pfnRead(low, high, pSector, pContext))
            continue;
        (*pTried)++;
        Dsk_ParseFatBoot(pSector, pBoot);
        if (pBoot->signature && pBoot->fat32 && pBoot->bytesPerSector == (512u << shift))
            return 1u << shift;
    }
    return 0;
}

#endif
