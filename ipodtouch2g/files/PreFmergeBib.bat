@echo off
rem
rem PreFmergeBib.bat - runs at the start of makeimg (ipodtouch2g BSP)
rem
rem makeimg calls this from the flat release directory before it merges the
rem .bib files and runs romimage. It removes the previous image so that
rem
rem  - romimage always runs: makeimg skips romimage (and with it
rem    PostMakeimg.bat, which creates wince.img3) when nk.bin is newer than
rem    its inputs, so nk.bin must go together with nk.nb0;
rem  - no stale parts remain: romimage writes nk.nb1 (and further parts) only
rem    when the image is larger than ROMSIZE and never deletes old ones, and a
rem    stale nk.nb1 would make PostMakeimg.bat report an oversized image;
rem  - no stale wince.img3 or wince.bin can be loaded by mistake.
rem
echo Running %0
pushd %_FLATRELEASEDIR%
if EXIST nk.bin del nk.bin
if EXIST nk.nb0 del nk.nb0
if EXIST nk.nb1 del nk.nb1
if EXIST nk.nb2 del nk.nb2
if EXIST nk.nb3 del nk.nb3
if EXIST wince.img3 del wince.img3
if EXIST wince.bin del wince.bin

rem
rem The touch controller's firmware and calibration (Apple's, kept out of
rem git in files\z2\, see docs\ipodtouch2g.md) go into the image under
rem the names platform.bib uses. Without them romimage stops with "file not
rem found": copy them there, or set BSP_NOIPTTOUCH=1.
rem
if .%BSP_NOIPTTOUCH%. == .1. goto touchdone
if EXIST z2fw.bin del z2fw.bin
if EXIST z2cal.bin del z2cal.bin
if EXIST "%_TARGETPLATROOT%\files\z2\z2fw-0x0051.bin" copy /y "%_TARGETPLATROOT%\files\z2\z2fw-0x0051.bin" z2fw.bin > nul
if EXIST "%_TARGETPLATROOT%\files\z2\z2cal.bin" copy /y "%_TARGETPLATROOT%\files\z2\z2cal.bin" z2cal.bin > nul
if NOT EXIST z2fw.bin echo PreFmergeBib: ERROR: %_TARGETPLATROOT%\files\z2\z2fw-0x0051.bin missing (touch firmware)
if NOT EXIST z2cal.bin echo PreFmergeBib: ERROR: %_TARGETPLATROOT%\files\z2\z2cal.bin missing (touch calibration)
:touchdone
popd
exit /b 0
