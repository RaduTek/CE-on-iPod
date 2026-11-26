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
// Before the reset the iPod's disk is told to write its cache to the
// platter (IOCTL_DISK_FLUSH_CACHE to DSK1:, which ipoddsk.dll passes on
// only if anything was written), so a reset right after a write loses
// nothing. Everything else of Windows CE lives in memory.
//
#include <windows.h>
#include <winioctl.h>
#include <pm.h>
#include <pkfuncs.h>
#include <diskio.h>

// The iPod's disk: its cache to the platter. Returns 0, or the error.
static DWORD FlushDisk(void)
{
    HANDLE h;
    DWORD got, err = 0;

    h = CreateFile(L"DSK1:", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError();
    if (!DeviceIoControl(h, IOCTL_DISK_FLUSH_CACHE, NULL, 0, NULL, 0, &got, NULL))
        err = GetLastError();
    CloseHandle(h);
    return err;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPWSTR pszCmdLine, int nCmdShow)
{
    WCHAR sz[128];
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

    // the disk's cache; a failure is shown, and the restart is left to the user
    err = FlushDisk();
    if (err != 0 && err != ERROR_FILE_NOT_FOUND && err != ERROR_DEV_NOT_EXIST)
    {
        wsprintf(sz, L"The iPod's disk did not confirm that it saved its cache (error %u). Restart anyway?",
                 err);
        if (MessageBox(NULL, sz, L"Restart", MB_YESNO | MB_ICONEXCLAMATION | MB_TOPMOST | MB_SETFOREGROUND) != IDYES)
            return 1;
    }

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
