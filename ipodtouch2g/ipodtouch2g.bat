@REM
@REM ipodtouch2g BSP environment (Windows Embedded Compact 7, iPod touch 2G / S5L8720)
@REM
@REM This file sets BSP-specific environment variables that are active during
@REM build. It defines the default state.
@REM

@ECHO OFF

rem
rem Touch: ipttouch.dll (src\drivers\ipttouch) with Microsoft's tchproxy.
rem BSP_NOTOUCH is not set, so common.reg/common.bib add tchproxy.dll and
rem tchcaldll.dll (they need the Touch Screen (Stylus) catalog item,
rem SYSGEN_TOUCH). To build without touch, set both BSP_NOTOUCH=1 and
rem BSP_NOIPTTOUCH=1 (see below).
rem
rem set BSP_NOTOUCH=1

rem
rem IMGFLASH=1 builds a flat RAM image (nk.nb0) linked at 0x80000000
rem (PA 0x08000000). PostMakeimg.bat wraps it into wince.img3 (an iBEC IMG3
rem for loadibec) and wince.bin (the raw image). startup.s moves the image
rem to PA 0x08000000 if iBoot started it somewhere else.
rem
set IMGFLASH=1

rem
rem Display: iptlcd.dll (src\drivers\iptlcd) on the framebuffer iBoot set
rem up. Set BSP_NODISPLAY=1 to use ddi_nop.dll instead. BSP_LCD_NOTESTPATTERN=1
rem skips the 1.5 s colour bars the driver shows at start.
rem
rem set BSP_NODISPLAY=1
rem set BSP_LCD_NOTESTPATTERN=1

rem
rem BSP_EARLYUART=1 makes startup.s print the hand-over probe on the dock
rem UART even in KITL builds (it is always on when IMGNOKITL=1). Only for
rem bring-up: the raw text arrives before Platform Builder syncs the KITL
rem stream.
rem
rem set BSP_EARLYUART=1

rem
rem BSP_NOSCREENMARKS=1 stops startup.s and OEMInit from filling the screen
rem with a colour per boot stage (blue: StartUp runs, green: KernelStart is
rem next, white: OEMInit runs). Rebuild the BSP after changing it.
rem
rem set BSP_NOSCREENMARKS=1

rem
rem BSP_NOTOUCHPROBE=1 leaves out the touch bring-up probe (touchprobe.c),
rem which dumps GPIO, SPI4 and the power chip's registers and tests the
rem touch controller's SPI link at the end of OEMInit (shown in builds
rem without KITL). Its job is done (2026-11-28) and the touch driver repeats
rem the reset sequence, so it is off. Rebuild the BSP after changing it.
rem
set BSP_NOTOUCHPROBE=1

rem
rem BSP_NOIPTTOUCH=1 leaves out the touch controller driver (ipttouch.dll)
rem and its firmware files. The driver needs files\z2\z2fw-0x0051.bin and
rem files\z2\z2cal.bin (Apple's, not in git; see docs\ipodtouch2g.md).
rem
rem set BSP_NOIPTTOUCH=1
