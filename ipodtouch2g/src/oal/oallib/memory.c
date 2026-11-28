//
// memory.c - memory related OAL functions for the ipodtouch2g BSP
//
// All RAM is described by config.bib / OEMAddressTable (128 MB DRAM, the
// top 4 MB kept out of CE's RAM).
//
#include <windows.h>
#include <oemglobal.h>

#include <bsp.h>

PCRamTable OEMGetOEMRamTable(void)
{
    return NULL;
}

BOOL OEMGetExtensionDRAM(LPDWORD lpMemStart, LPDWORD lpMemLen)
{
    UNREFERENCED_PARAMETER(lpMemStart);
    UNREFERENCED_PARAMETER(lpMemLen);
    return FALSE;
}

DWORD OEMEnumExtensionDRAM(PMEMORY_SECTION pMemSections, DWORD dwMemSections)
{
    UNREFERENCED_PARAMETER(pMemSections);
    UNREFERENCED_PARAMETER(dwMemSections);
    return 0;
}

DWORD OEMCalcFSPages(DWORD dwMemPages, DWORD dwDefaultFSPages)
{
    UNREFERENCED_PARAMETER(dwMemPages);
    return dwDefaultFSPages;
}

DWORD OEMReadRegistry(DWORD dwFlags, LPBYTE pBuf, DWORD len)
{
    UNREFERENCED_PARAMETER(dwFlags);
    UNREFERENCED_PARAMETER(pBuf);
    UNREFERENCED_PARAMETER(len);
    return 0;
}

BOOL OEMWriteRegistry(DWORD dwFlags, LPBYTE pBuf, DWORD len)
{
    UNREFERENCED_PARAMETER(dwFlags);
    UNREFERENCED_PARAMETER(pBuf);
    UNREFERENCED_PARAMETER(len);
    return TRUE;
}

BOOL OEMIsRom(DWORD dwShiftedPhysAddr)
{
    UNREFERENCED_PARAMETER(dwShiftedPhysAddr);
    return FALSE;
}

void OEMNotifyForceCleanBoot(DWORD dwArg1, DWORD dwArg2, DWORD dwArg3, DWORD dwArg4)
{
    UNREFERENCED_PARAMETER(dwArg1);
    UNREFERENCED_PARAMETER(dwArg2);
    UNREFERENCED_PARAMETER(dwArg3);
    UNREFERENCED_PARAMETER(dwArg4);
    return;
}
