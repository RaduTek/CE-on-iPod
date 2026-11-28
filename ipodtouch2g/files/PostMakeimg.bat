@echo off
rem
rem PostMakeimg.bat - runs after makeimg (ipodtouch2g BSP)
rem
rem Wraps nk.nb0 (without its padding) for the patched iBoot:
rem   wince.img3 - IMG3 container of type iBEC, for loadibec (recovery mode)
rem   wince.bin  - the same payload without a container
rem
rem If files\template-ipt2g.img3 exists (copy it from openiBoot,
rem mk8900image\template-ipt2g.img3), wince.img3 keeps all of its tags and
rem only the DATA tag is replaced, as openiBoot's own build does. Without the
rem template a minimal IMG3 (TYPE and DATA only) is written.
rem
rem The image is packed (LZ4, unpacked by startup.s) because iBSS takes at
rem most 16 MiB over USB. BSP_NOCOMPRESS=1 writes it unpacked (only works
rem for images that fit).
rem
echo Running %0

set MKIMG3_PACK=-Compress
if .%BSP_NOCOMPRESS%. == .1. set MKIMG3_PACK=

if .%IMGFLASH%. == .. goto NoFlash

pushd %_FLATRELEASEDIR%

rem Check that the image fits in the NK region (32 MB)
if NOT EXIST nk.nb1 goto sizegood
echo.
echo Error! nk.nb1 exists: the image is larger than NKLEN (32 MB) in config.bib.
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
if EXIST wince.img3 del wince.img3
if EXIST wince.bin del wince.bin
echo Creating wince.img3 and wince.bin from nk.nb0
if EXIST "%_TARGETPLATROOT%\files\template-ipt2g.img3" goto withtemplate
echo Note: files\template-ipt2g.img3 not found, writing a minimal IMG3
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%_TARGETPLATROOT%\files\mkimg3.ps1" -InFile nk.nb0 -OutFile wince.img3 -RawFile wince.bin %MKIMG3_PACK%
goto mkdone
:withtemplate
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%_TARGETPLATROOT%\files\mkimg3.ps1" -InFile nk.nb0 -OutFile wince.img3 -RawFile wince.bin -Template "%_TARGETPLATROOT%\files\template-ipt2g.img3" %MKIMG3_PACK%
:mkdone
if ERRORLEVEL 1 goto mkfail
if NOT EXIST wince.img3 goto mkfail
popd
goto endit

:mkfail
echo.
echo Error! mkimg3.ps1 failed to create wince.img3
echo.
popd
goto errorend

:NoFlash
echo.
echo IMGFLASH not set. It must be set to create wince.img3
echo.
:errorend
echo Setting error level to indicate error to makeimg
exit /b 1
:endit
