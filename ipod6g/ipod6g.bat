@REM
@REM ipod6g BSP environment (Windows Embedded Compact 7, iPod classic / S5L8702)
@REM
@REM This file sets BSP-specific environment variables that are active during
@REM build. It defines the default state.
@REM

@ECHO OFF

rem
rem No touch screen on the iPod.
rem
set BSP_NOTOUCH=1

rem
rem IMGFLASH=1 builds a flat RAM image (nk.nb0) linked at 0x80000000
rem (PA 0x08000000). PostMakeimg.bat wraps it into rockbox.ipod, which the
rem Rockbox bootloader loads from the HDD and jumps to.
rem
set IMGFLASH=1

rem
rem Display: ipodlcd.dll (src\drivers\ipodlcd). Set BSP_NODISPLAY=1 to use
rem ddi_nop.dll instead. BSP_LCD_NOTESTPATTERN=1 skips the 1.5 s colour bars
rem the driver shows at start.
rem
rem set BSP_NODISPLAY=1
rem set BSP_LCD_NOTESTPATTERN=1

rem
rem BSP_EARLYUART=1 makes startup.s print "CE>" on the dock UART even in
rem KITL builds (it is always on when IMGNOKITL=1). Only for bring-up: the
rem raw text arrives before Platform Builder syncs the KITL stream.
rem
rem set BSP_EARLYUART=1
