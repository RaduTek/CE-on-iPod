//
// restart.c - leave Windows CE by restarting the iPod
//
// "Restart" on the desktop. After a yes it closes a music player if one runs (so
// no sound is cut off in the middle), then asks Windows' power manager for a
// reset. The power manager passes that to the kernel layer (IOCTL_HAL_REBOOT
// in src\oal\oallib\ioctl.c), which resets the processor through its
// watchdog, the way Rockbox itself restarts. The iPod then starts as from a
// reset: the boot loader, then whatever it starts.
//
// Nothing has to be saved first: Windows CE has the iPod's disk read-only,
// and everything else of it lives in memory.
//
#include <windows.h>
#include <pm.h>
#include <pkfuncs.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR pszCmdLine, int nCmdShow)
{
    WCHAR sz[96];
    HWND hPlayer;
    DWORD err;
    int i;

    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(pszCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    if (MessageBox(NULL, L"Restart the iPod?", L"Restart",
                   MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES)
        return 0;

    // the music first
    hPlayer = FindWindow(L"IpodMusicPlayer", NULL);
    if (hPlayer != NULL)
    {
        PostMessage(hPlayer, WM_CLOSE, 0, 0);
        for (i = 0; i < 20 && IsWindow(hPlayer); i++)
            Sleep(100);
    }
    Sleep(300);

    // this does not come back when it works
    SetSystemPowerState(NULL, POWER_STATE_RESET, POWER_FORCE);
    err = GetLastError();
    Sleep(1500);

    // still here: ask the kernel layer itself
    KernelIoControl(IOCTL_HAL_REBOOT, NULL, 0, NULL, 0, NULL);
    Sleep(1500);

    wsprintf(sz, L"The restart did not happen (error %u, then %u).", err, GetLastError());
    MessageBox(NULL, sz, L"Restart", MB_OK | MB_ICONEXCLAMATION | MB_TOPMOST | MB_SETFOREGROUND);
    return 1;
}
