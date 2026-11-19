//
// vector.c - VFP hooks for the ipod6g BSP
//
// The ARM926EJ-S has no VFP. These stubs satisfy the OEMGlobal VFP entries
// in case the kernel's default table references them.
//
#include <windows.h>
#include <winnt.h>
#include <oemglobal.h>

#ifdef ARM

void OEMSaveVFPCtrlRegs(LPDWORD lpExtra, int nMaxRegs)
{
    UNREFERENCED_PARAMETER(lpExtra);
    UNREFERENCED_PARAMETER(nMaxRegs);
    return;
}

void OEMRestoreVFPCtrlRegs(LPDWORD lpExtra, int nMaxRegs)
{
    UNREFERENCED_PARAMETER(lpExtra);
    UNREFERENCED_PARAMETER(nMaxRegs);
    return;
}

BOOL OEMHandleVFPException(ULONG fpexc, EXCEPTION_RECORD *pExr, CONTEXT *pContext,
                           DWORD *pdwExcpId, BOOL fInKMode)
{
    UNREFERENCED_PARAMETER(fpexc);
    UNREFERENCED_PARAMETER(pExr);
    UNREFERENCED_PARAMETER(pContext);
    UNREFERENCED_PARAMETER(pdwExcpId);
    UNREFERENCED_PARAMETER(fInKMode);
    return FALSE;
}

BOOL OEMIsVFPFeaturePresent(DWORD dwProcessorFeature)
{
    UNREFERENCED_PARAMETER(dwProcessorFeature);
    return FALSE;
}

BOOL OEMVFPCoprocControl(DWORD dwCommand)
{
    UNREFERENCED_PARAMETER(dwCommand);
    return FALSE;
}

#endif
