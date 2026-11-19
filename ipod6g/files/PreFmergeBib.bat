@echo off
rem
rem PreFmergeBib.bat - runs at the start of makeimg (ipod6g BSP)
rem
rem makeimg calls this from the flat release directory before it merges the
rem .bib files and runs romimage. It removes the previous image so that
rem
rem  - romimage always runs: makeimg skips romimage (and with it
rem    PostMakeimg.bat, which creates wince.ipod) when nk.bin is newer than
rem    its inputs, so nk.bin must go together with nk.nb0;
rem  - no stale parts remain: romimage writes nk.nb1 (and further parts) only
rem    when the image is larger than ROMSIZE and never deletes old ones, and a
rem    stale nk.nb1 would make PostMakeimg.bat report an oversized image;
rem  - no stale wince.ipod can be copied to the iPod by mistake.
rem
echo Running %0
pushd %_FLATRELEASEDIR%
if EXIST nk.bin del nk.bin
if EXIST nk.nb0 del nk.nb0
if EXIST nk.nb1 del nk.nb1
if EXIST nk.nb2 del nk.nb2
if EXIST nk.nb3 del nk.nb3
if EXIST wince.ipod del wince.ipod
if EXIST rockbox.ipod del rockbox.ipod
popd
exit /b 0
