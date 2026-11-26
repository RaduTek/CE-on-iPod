# CE-on-iPod: Windows Embedded Compact 7 BSP for the iPod classic

## What this project is
A Board Support Package (BSP) that runs Windows Embedded Compact 7 (CE 7) on the iPod classic
(6th/7th generation, Samsung S5L8702, ARM926EJ-S, 64 MB SDRAM). The Rockbox bootloader
(already flashed to the iPod's NOR) loads the kernel from the HDD. Debug and KITL run over the
UART exposed on the 30-pin dock connector.

**Current goal:** boot the kernel and connect Platform Builder over serial KITL. After that:
display, HDD, PMU/backlight, clickwheel, audio.

## How we work (important)
- Claude Code runs on the host PC in this repo. It can read and edit files but **cannot build or
  run anything**.
- The build happens in a Windows 7 VM (Platform Builder 7 + Visual Studio 2008 SP1, ARMv5
  support installed) at `C:\WINCE700`. `ipod6g/` must appear as `C:\WINCE700\platform\ipod6g`.
  `iPodCE/` is the sample OS design.
- The VM's `C:\WINCE700` is mapped on the host as `Z:` (read-only reference).
- The user builds, copies `wince.ipod` to the iPod disk and reports results (serial log, KITL
  output, build logs). **Since 2026-11-26 the user tests by launching `wince.ipod` from Rockbox
  (ROLO)**, which is easier and needs no custom bootloader. The PLAY start (custom bootloader,
  docs/bootloader.md) must keep working but is not routinely tested. A ROLO start leaves hardware
  that Rockbox set up (codec, I2S, wheel); the bootloader does not set up the codec/I2S. Drivers
  should set up what they need rather than rely on either.
- Rockbox source: `C:\dev\rockbox` (branch `ce-on-ipod-loadsize`), not `../rockbox/`.
- After every change, say exactly what to build, what to expect, and which log lines or
  behaviour confirm success. Prefer small, separately testable changes.
- Never claim hardware behaviour that has not been verified. State assumptions and say how to
  test them. If a register or bit is uncertain, say so.

## File rules (the build tools are old and fragile)
- Build-system and source files must use **CRLF** line endings and plain ASCII, with a trailing
  newline: `dirs`, `sources`, `makefile`, `sources.cmn`, `*.c`, `*.h`, `*.s`, `*.inc`, `*.bib`,
  `*.reg`, `*.dat`, `*.bat`, `*.ps1`, `*.pbxml`, `*.pbcxml`, `*.sln`. `.gitattributes` enforces this
  on checkout. Check new files for LF-only line endings.
- Avoid exotic `sources` syntax, very long lines and non-ASCII characters (`build.exe` is old).
- `files/mkrbipod.ps1` runs in the VM on **Windows PowerShell 2.0**: no `-shl`/`-shr`, no
  PS3+ cmdlets; use `[Environment]::Exit(n)` for exit codes (plain `exit n` returns 0 under
  `-File` on 2.0). Test with `powershell -Version 2` on the host.
- Do not change the memory map (`files/config.bib`, `src/inc/OEMAddrTable.inc`,
  `src/inc/image_cfg.h`) without asking.

## Repository layout
- `ipod6g/` (the BSP, = `C:\WINCE700\platform\ipod6g`)
  - `ipod6g.bat`: BSP environment. `IMGFLASH=1`, `BSP_NOTOUCH=1`, `BSP_NODISPLAY=1`.
  - `Catalog/ipod6g.pbcxml`: `Bsp:CE-on-iPod:ipod6g:ARMV5`, `Cpu:ARMV5`.
  - `files/`: `config.bib` (memory map), `platform.bib/.reg`, `PostMakeimg.bat` +
    `mkrbipod.ps1` (nk.nb0 -> wince.ipod).
  - `src/inc/`: `s5l8702.h/.inc` (registers), `s5l8702_uart.h` (shared UART0 code),
    `OEMAddrTable.inc`, `image_cfg.h`, `bsp_cfg.h` (static SYSINTRs), `bsp.h`.
  - `src/oal/oallib/`: `startup.s`, `init.c` (OEMInit), `debug.c`, `intr.c` (VICs), `timer.c`,
    `ioctl.c`, `memory.c`, `power.c`, `rtc.c`, `vector.c`.
  - `src/oal/oalexe/`: links `oal.exe` (nk.exe) with `nkldr`, `oemmain`, `oal_cache_arm926`, ...
  - `src/kitl/`: `kitl.dll`, serial KITL via the stock `oal_kitl.lib` + `kitlSerDrv.c`.
- `iPodCE/`: sample OS design (headless: kernel, Target Control shell, toolhelp).
- `docs/bringup.md`: boot chain, wiring, build and test steps. `docs/bootloader.md`: Rockbox
  bootloader build (32 MiB, PLAY -> /wince.ipod); the patch is `rockbox/0001-*.patch` (`git am`).

## Reference trees (read-only)
- `../rockbox/`: hardware facts for everything (S5L8702 registers, LCD, ATA, PMU, codec).
  Key files: `firmware/export/s5l87xx.h`, `firmware/export/uc87xx.h`,
  `firmware/target/arm/s5l8702/`, `bootloader/ipod-s5l87xx.c`, `firmware/common/rb-loader.c`.
  Rockbox is GPL: use it as documentation, do not copy code verbatim.
- `../ceonpi/`: the CE 7 Raspberry Pi BSP this BSP's structure comes from (same workflow).
- `Z:\platform\3DS_iMX27`: Microsoft's ARMv5 (ARM926, i.MX27) BSP - reference for anything
  ARMv5-specific (processor type, config.bib, link lines, serial KITL).
- `Z:\platform\common\src\ARM\ARM926`: ARM926 cache library used by the OAL.
- `Z:\platform\SMDK6410`, `Z:\platform\common\src\soc\S3C6410_SEC_V1`: Samsung UART/VIC code.

## Boot chain and hardware facts (from Rockbox sources; verify on hardware)
- Our Rockbox bootloader build (docs/bootloader.md, Rockbox branch `ce-on-ipod-loadsize`) loads
  `/wince.ipod` (disk root, no fallback) when PLAY is held at power-on, else Rockbox as usual;
  the stock bootloader only loads `/.rockbox/rockbox.ipod` (fallback `/rockbox.ipod`) from the FAT32
  volume: 8-byte header (BE32 `71 + sum(payload bytes)`, then `"ip6g"`, not checked), payload
  copied to **PA 0x08000000**, max **8 MiB** (stock) or **32 MiB** (our bootloader build,
  docs/bootloader.md), then branches to the first byte. mkrbipod.ps1 trims nk.nb0 to the
  real image end (ROMHDR.physlast), so the file is the image size, not ROMSIZE.
- Hand-over state: SYS mode, IRQ/FIQ masked, **MMU + I/D caches on, identity map**, VIC sources
  enabled, Timer B ticking, UART clock gated, HDD spun up, LCD initialised, backlight on.
- Holding MENU at power-on boots the Apple OS (recovery path).
- Memory: SDRAM 0x08000000-0x0BFFFFFF; I/O 0x38000000-0x3FFFFFFF; IRAM0 0x22000000.
- UART0 (dock): 0x3CC00000, clock gate PWRCON1 (0x3C50004C) bit 9, pins PCON0[23:16]=0x22,
  ECLK 12 MHz, UBRDIV=6 + UBRCONTX/RX=0xFFFFFF = ~114286 baud. VIC0 IRQ 24.
- VICs: 0x38E00000 / 0x38E01000. Timer B (tick) IRQ 8, Timer E free-running 1 MHz (TECNT
  0x3C7000B4). Watchdog reset: write 0x100000 to 0x3C800000.
- Dock pins (inferred, not verified): 11 = GND, 12 = iPod TX, 13 = iPod RX, 3.3 V levels.

## Debugging notes
- `IMGNOKITL=1` (Release config) builds print `CE>` from `startup.s`, then the
  `OEMInitDebugSerial` banner with the build date, then kernel output, all raw on UART0.
- KITL builds (Debug/Checked) keep UART0 silent until Platform Builder connects;
  `BSP_EARLYUART=1` forces the `CE>` marker for bring-up.
- makeimg uses the copy of `platform.reg`/`.bib` in the flat release dir: after editing them run
  "Copy Files to Release Directory" before makeimg (lesson from CEonPi).

## Progress
- 2026-11-19: **Checked image boots** from `rockbox.ipod` with serial KITL connected to Platform
  Builder (KdStub attached, symbols load). Kernel reaches `shell.exe`; filesys, romfsd, relfsd
  (`\Release`) mount. Memory: 15494 pages (~60 MB) usable. Image data relocated at 0x802f6000
  (image ~3 MB; nb0 is padded to ROMSIZE = 8 MB). Confirms: startup.s hand-over, address table,
  timer tick, VIC handling, UART0 KITL driver.
- Boot log quirks that are expected: tick counts start at 4294767296 (the kernel starts CurMSec
  at -200000 to expose wrap bugs); "Download service ... NONE" (image is not downloaded);
  "Mutually dependent DLL detected: FPCRT" (stock).
- Fixed after first boot: "Platform Name = Undefined" (added IOCTL_HAL_GET_DEVICE_INFO
  handler) and "Failed to load device.dll" (added `Item:MS:sysgen_device` to the OS design).
- 2026-11-19 (second boot): `Platform Name = ipod6g`; device.dll, devmgr.dll, k.ceddk.dll and
  busenum.dll load. Target Control works: `gi proc` lists NK.EXE and shell.exe; `mi` reports
  13081 pages, 11313 free (~44 MB free). BSP builds with 0 warnings (the ~26 build warnings come
  from other SoCs' libraries in platform\common, e.g. ARM11 `cps` in mxarm11/S3C6410 cache libs).
- `OEMInterruptEnable: unknown SysIntr 11` = **SYSINTR_IST_BGT** (SYSINTR_DEVICES+3), the
  kernel's background IST for unknown IRQs; software-only, no hardware IRQ. intr.c now accepts
  all SYSINTRs below SYSINTR_FIRMWARE (except RESCHED) as no-ops. (Same message is unexplained
  in the CEonPi notes; this is its cause there too.)
- 2026-11-26: **Shared I2C driver `ipodi2c.dll` (`I2C1:`, Microsoft i2c.h interface) verified**
  from both ROLO and cold (PLAY) start: bus pins `33331222` both ways, the power chip's clock read
  and battery measurement (writes to 0x52-0x54) work through it. battdrvr is now its client.
- 2026-11-26: **cold start vs ROLO compared**: disk, LCD (same `LCD_CON 0x80100db0`), click wheel
  (bootloader's `button_init` starts it), I2C, battery all work both ways. Only sound differs:
  cold start gives `ipodwav: I2SCLKDIV is 0` and the driver does not load (the bootloader never
  sets up the codec/I2S). `DEVICE!InitDeviceContent ... 'Notify', error code 2404` appears in
  both (Microsoft's notify driver, not ours).
- 2026-11-26: **Sound works from a cold start.** ipodpcm.c now sets up I2S, the codec clock and
  reset, and Rockbox's CS42L55 register sequence through I2C1: on every start. Log on both
  starts: `ipodwav: codec set up: version 0xe9, 33 registers, headphones 0x6c (-20 dB),
  I2SCLKDIV was 272` (ROLO) / `was 0` (cold), then `ready, I2SCLKDIV 272 = 44118 Hz`. The codec
  answers after reset on a cold start, so its supply is on without any power-chip change.
  Load order I2C1: (0) -> battery (1) -> sound (2) confirmed in the log.
- **Next (planned 2026-11-26): port the sound driver to Microsoft's wave MDD + our PDD** (CEonPi
  model, drops `getms.ps1`). Full plan: `docs/audio-mdd-plan.md`.
- 2026-11-26: **DMAC0 interrupt verified** (plan step 1, ROLO): `SYSINTR_AUDIO` (17) <-> IRQ 16,
  16 x 256-frame DMA ring, test tone 168 interrupts in 1004 ms, 0 timeouts/errors. Rockbox leaves
  a stale TC bit on DMAC0 channel 4 (its LCD DMA) after ROLO; ring start clears idle channels'
  status. Synchronous RETAILMSG costs ~3 ms per 40 characters (114 kbaud): keep it out of
  timing-critical paths.

## Display driver (src/drivers/ipodlcd) - test pattern verified on hardware 2026-11-19
- `ipodlcd.dll`, GPE flat driver (structure from CEonPi raspiBasic_ddi), loaded by GWES (SHMK).
  Fixed mode 320x240 RGB565, no OAL IOCTL_HAL_DDI, no pointer.
- The panel has its own GRAM (8080 MPU bus); there is no scanned framebuffer. GDI draws into a
  RAM shadow surface; BltPrepare/Line record a dirty rectangle, BltComplete/lines signal a flush
  thread that waits 15 ms (to batch drawing) and pushes the rectangle with CPU writes
  (`lcdhw.cpp`). DMA (PL080 DMAC0 0x38200000, peripheral 3 = LCD_WR, dest LCD_BASE+0x40) later.
- Panel protocol (Rockbox lcd-s5l8702.c / lcd-6g.c): LCD_BASE 0x38300000; panel type =
  PDAT6[5:4] (0x3CF000C4). Types 0/1: 8-bit command set (0x2A/0x2B/0x2C), command mode
  LCD_CON=0x80000C20. Types 2/3: 16-bit registers 0x210-0x213, 0x200/0x201, 0x202, command mode
  0x80000DA8. Pixel data in frame mode 0x80100DB0, one RGB565 pixel per LCD_WDATA (+0x40) write.
  Wait STATUS&0x2 before writing LCD_CON, wait !(STATUS&0x10) before WCMD/WDATA.
- Panel init sequences only run in the Rockbox bootloader; the driver relies on that.
- Bring-up: colour bars for 1.5 s at SetMode (disable with BSP_LCD_NOTESTPATTERN=1).
- OS design needs `sysgen_minwmgr` and `sysgen_display`. Segoe UI (0.5 MB) comes with GWES by
  default; do not add Tahoma (0.7 MB): with it the Checked image is ~8.3 MB, over the stock 8 MiB limit (limit is 32 MiB with docs/bootloader.md).

## Image packaging notes (2026-11-19)
- makeimg runs `PreFmergeBib.bat` and `PostMakeimg.bat` from the **flat release dir**, not from
  `ipod6g\files`. After changing either (or `mkrbipod.ps1` comments), run "Copy Files to Release
  Directory" (or a full build) so the release dir gets the new copies.
- `NK.nb1`: romimage writes nb0, nb1, ... in ROMSIZE-sized parts when the image is bigger than
  ROMSIZE, and never deletes old parts. A stale `NK.nb1` from the 8 MB-NK days (8388608 bytes,
  older than NK.nb0) made PostMakeimg fail after the NK region grew to 32 MB.
  `PreFmergeBib.bat` (runs before romimage) now deletes `nk.bin`, `nk.nb0-3`, `wince.ipod`,
  `rockbox.ipod`. `nk.bin` must go too: makeimg skips romimage AND PostMakeimg (RunRomImage
  target, Outputs=nk.bin) when nk.bin is newer than its inputs, e.g. on a plain `makeimg` rerun.
- With NKLEN = 32 MB, `NK.nb0` is always 33554432 bytes; `wince.ipod` is the trimmed image
  (Checked + GWES + display: NK.bin ~8.4 MB, so ~8.3 MB, needs the 32 MiB bootloader).
- 2026-11-19 (third boot, Checked + GWES): **LCD verified**: colour bars displayed correctly.
  Log: `panel type 1 (8-bit command set), LCD_CON 0x80100db0, STATUS 0x00001086, PHTIME 0x33`
  (bootloader leaves the controller in frame mode), framebuffer 0xc7f20010 stride 640,
  `GWES initialized properly`. `Could not find touch driver dll` is expected (BSP_NOTOUCH).
  Image data relocated at 0x80816000 (image > 8 MiB), so the 32 MiB bootloader build loaded it.
  Tahoma is still in the image despite removing `sysgen_fonts_tahoma` from the pbxml - check the
  catalog selection if the 0.7 MB matters.
- Resolved (2026-11-19): after the bars the screen stayed on the bars because GWES alone (MinWMGR,
  no shell) draws nothing: `status 5 s after SetMode: 0 blts and 0 lines`. lcdtest verified the
  GDI -> blit -> dirty rect -> flush path on hardware (full screen flush 33 ms, 200x20 strip
  1-2 ms). Oddity: lcdtest took ~16 s instead of ~5 s (first 2 loop iterations ~6.6 s), cause
  unknown (paging, KITL/debugger, or Sleep timing) - check if UI feels slow.
  ipodlcd now logs the first blits/flushes and a status line 5 s after SetMode
  (`ipodlcd: status 5 s after SetMode: N blts ...`). `src/apps/lcdtest` (lcdtest.exe, in the
  image with the display) draws boxes/line/text on the screen DC: Target Control `s lcdtest`.
- Standard Shell added to the OS design (2026-11-19): `sysgen_standardshell` (+ commctrl,
  commdlg, ctlpnl listed explicitly; wceshellfe.bat also turns on full GWES, OLE, IMM, PM).
  Explorer starts via `Launch50="explorer.exe"` in public\shell\oak\files\shell.reg. No input
  driver yet: the desktop is view-only until the clickwheel driver exists.
