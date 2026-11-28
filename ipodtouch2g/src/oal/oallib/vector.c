//
// vector.c - VFP hooks for the ipodtouch2g BSP
//
// OEMInit calls VfpOemInit (oal_vfp_arm.lib), which installs the real VFP
// handlers in OEMGlobal. These stubs only satisfy references to the default
// names, as in the CEonPi BSP.
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
