@echo off
rem
rem PostMakeimg.bat - runs after makeimg (ipod6g BSP)
rem
rem Wraps nk.nb0 into wince.ipod: an 8-byte header (big-endian checksum
rem 71 + sum of payload bytes, then "ip6g") followed by the CE image
rem (nk.nb0 without its padding). The CE-on-iPod Rockbox bootloader loads
rem /wince.ipod (disk root) to 0x08000000 when PLAY is held at power-on, and
rem jumps to it. With the stock bootloader, copy it as /.rockbox/rockbox.ipod.
rem
echo Running %0

if .%IMGFLASH%. == .. goto NoFlash

pushd %_FLATRELEASEDIR%

rem Check that the image fits in the NK region (32 MB, the bootloader limit)
if NOT EXIST nk.nb1 goto sizegood
echo.
echo Error! nk.nb1 exists: the image is larger than NKLEN (32 MB) in config.bib.
echo The CE-on-iPod Rockbox bootloader cannot load more than 32 MiB.
echo.
popd
goto errorend

:sizegood
if EXIST nk.nb0 goto havenb0
echo.
echo Error! nk.nb0 not found in %_FLATRELEASEDIR%
echo.
popd
goto errorend

:havenb0
if EXIST wince.ipod del wince.ipod
echo Creating wince.ipod from nk.nb0
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%_TARGETPLATROOT%\files\mkrbipod.ps1" -InFile nk.nb0 -OutFile wince.ipod
if ERRORLEVEL 1 goto mkfail
if NOT EXIST wince.ipod goto mkfail
popd
goto endit

:mkfail
echo.
echo Error! mkrbipod.ps1 failed to create wince.ipod
echo.
popd
goto errorend

:NoFlash
echo.
echo IMGFLASH not set. It must be set to create wince.ipod
echo.
:errorend
echo Setting error level to indicate error to makeimg
exit /b 1
:endit
