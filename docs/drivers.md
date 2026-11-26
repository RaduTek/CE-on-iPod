# Drivers for the iPod's own hardware

What each driver expects to find, what it does and does not do, and how to
see what it found. All of them were developed and tested on an iPod classic
6G with an iFlash adapter, with Windows CE **started from Rockbox**: a menu
entry of a Rockbox build of my own loads `wince.ipod` with Rockbox's firmware
loader (ROLO). Opening `wince.ipod` from a stock Rockbox's file browser goes
through the same loader but was not tried. None of the drivers sets the
hardware up from scratch: they find it as Rockbox left it. **None has been
tried with the bootloader's PLAY start**, where Rockbox never runs; sound and
the click wheel in particular will need their own set-up there (see each
"Expects").

## Sound: `ipodwav.dll` (`src\drivers\wavedev`)

- Microsoft's prebuilt wave MDD (`wavemdd.lib`, linked from the public
  tree: the `WAV_*` entry points, wave API messages, header queue and the
  interrupt thread) with our PDD (`wavepdd.cpp`) over the iPod's audio path
  (`ipodpcm.c`, interface `pcmring.h`). The model is CEonPi's analog audio
  driver. Windows' software mixer (in `waveapi.dll`) mixes all programs and
  converts their rates: the driver takes 44100 Hz only, PCM 8/16-bit,
  mono/stereo, one stream (the mixer's).
- The path: DMA controller 0, channel 2, into I2S 0, into the CS42L55 codec;
  24-bit samples in 32-bit words. A DMA ring of 16 blocks x 256 frames
  (~93 ms) with an interrupt per block (`SYSINTR_AUDIO` = 17, IRQ 16, every
  ~5.8 ms): the MDD's thread (`Priority256` 110) clears the block just
  played to silence and copies more audio ahead of the DMA. Buffers go back
  to the mixer as soon as they are copied.
- DMA controller 0 has one interrupt line for all 8 channels. Rockbox's LCD
  DMA (channel 4) is left with a finished transfer pending and its interrupt
  on, after ROLO and after the bootloader alike (log: `pending on idle
  channels: 0x10`); that held IRQ 16 up and stopped the boot once. The
  driver therefore switches every other channel's interrupts off and clears
  idle channels' pending bits at start-up, before the MDD enables IRQ 16,
  and again on any interrupt while nothing plays (`spurious` in Sound
  check). A second user of DMA controller 0 (e.g. LCD DMA) needs the OAL to
  tell the channels apart, or moves to DMA controller 1.
- **Sets up the path itself at start-up**, the same after a Rockbox (ROLO)
  start and a cold start (where the bootloader leaves the codec in reset),
  in Rockbox's order: I2S 0 clock and transmitter, `I2SCLKDIV` 272
  (44118 Hz), the codec's master clock (`CLKCON3`), its reset (port 3 pin
  3) low 10 ms, then through `I2C1:` the codec's version register and
  Rockbox's `audiohw_preinit`/`postinit` register sequence. Headphones at
  -20 dB (`IPODPCM_HP_DB`), unmuted. Loads after `I2C1:` (`Order` 2).
- **Volume on the codec** (registry `CodecVolume` 1, the default): Windows'
  volume sets the headphone levels (HPACTL left, HPBCTL right) through
  `I2C1:` in 1 dB steps below -20 dB, down to the codec's -60 dB; 0 mutes.
  The samples stay at full scale, so quiet sounds keep their resolution.
  `CodecVolume` 0 scales the samples instead, as before.
- **Expects:** the I2S pins set up (Rockbox's GPIO table, which the
  bootloader applies too) and the codec's supply on (Rockbox does not know
  which power chip output feeds it either).
- **Does not:** change the rate, detect headphones, record.
- **Check:** "Sound check" on the desktop (ring interrupts, late ones,
  underruns, the stream's format, DMA errors; a test tone and the start-up
  sound). Registry `TestTone` 1 plays 440 Hz left then 880 Hz right at
  start-up and logs how the ring's interrupts came (`test tone done: ...`).

## Disk, read only: `ipoddsk.dll` (`src\drivers\ipoddsk`)

- `DSK1:`, a block device for the storage manager, on top of `ipodata.c`:
  the S5L8702's ATA controller in PIO mode, IDENTIFY and READ SECTORS only.
  About 1.9 MB/s raw, 1.8 MB/s through the file system (iFlash).
- Read only twice over: `ipodata.c` has no write command at all, and every
  write, format and delete request is refused before it is called. The
  storage manager is told the store is read only (`Attrib` 1), so programs
  get "write protected" from Windows.
- The disk answers in 512-byte sectors, but an iTunes-formatted iPod has a
  FAT32 file system in 4096-byte sectors: at start-up the driver finds the
  FAT32 volume (`dskparse.h`) and shows Windows a disk of the sector size
  the file system uses. Mounted as `\iPod`.
- **Expects:** the disk powered and spun up, its pins set up, the controller
  enabled (Rockbox has just read the image from it). PATA only: a CE-ATA
  disk (later models) is refused with its own error code (written from
  Rockbox's sources; no CE-ATA iPod was at hand to try).
- **Does not:** reset, power or configure the disk; no DMA; no sleep.
- **Check:** "iPod disk" on the desktop (the volume found, reads and
  refused writes, the last error and the step it happened at, the stores
  Windows lists; "Verify file" reads `\iPod\wince.ipod` to its end and
  checks its checksum and speed).

## I2C bus 0: `ipodi2c.dll` (`src\drivers\ipodi2c`)

- `I2C1:`, the only code that drives I2C controller 0. The power chip
  (PCF50635, 7-bit address 0x73) and the codec (CS42L55, 0x4A) share this
  bus, so every driver that talks to either goes through `I2C1:`, one
  transfer at a time.
- Microsoft's I2C interface (`public\common\oak\inc\i2c.h`): `I2cOpen`,
  `I2cSetSlaveAddress`, `I2cRead`/`I2cWrite` at a register. As with
  Microsoft's own I2C driver, only kernel-mode drivers may open it. 7-bit
  addresses and 1-byte register numbers only; the speed and timeout settings
  are accepted but not used.
- The controller code (`i2chw.c`) is the battery driver's former bus layer:
  polled, the controller's clock on only during a transfer, a read is
  register number + stop + a new start (no repeated start), every wait
  bounded, and the bus is released after a failure. Writes are new.
- **Allowlist** (`g_allow` in `ipodi2c.c`): only the two chips above.
  The power chip takes writes only to 0x52-0x54 (battery measurement), so
  its supply and charger settings cannot be changed by a wrong caller. The
  codec takes writes to any register. Anything else is refused before the
  bus is touched (`ERROR_ACCESS_DENIED`, counted as "refused").
- `IOCTL_IPODI2C_GET_STATE` (`ipodi2c.h`): reads, writes, errors, refusals,
  and the chip, register, step and code of the last failure. The first 16
  failures are also written to the debug output (`ipodi2c: ... failed`).
- **Expects:** the bus pins set up (PCON3) as Rockbox and the bootloader
  leave them. It never sets them itself: with other pins it refuses every
  transfer (code 2).

## Clock and battery: `battdrvr.dll` (`src\drivers\battery`)

- Microsoft's battery driver library (`battdrvr_mdd.lib`) with the iPod's
  half in `ipodbatt.c`, reaching the power chip through `I2C1:`.
- Clock: the power chip's clock is read once at start-up and given to
  Windows (`SetLocalTime`); after that Windows' clock runs on the kernel's
  tick. The chip's clock is never set.
- Battery: the voltage is measured the way Rockbox does (three fixed bytes
  to the chip's measuring registers 0x52, 0x53, 0x54, the only writes this
  driver makes), at most every 20 seconds, and turned into a percentage
  with Rockbox's table for this iPod. "External power" is the chip's
  charger status, "charging" the processor pin Rockbox uses for it.
- `battray.exe`: the battery icon in the taskbar.
- **Expects:** `I2C1:` loaded first (registry `Order` 0, the battery
  driver 1). If it is not there, the clock is read on the first status
  request that can open it.
- **Check:** "Power status" on the desktop (Windows' clock and power
  status, the chip's clock as read at start-up, measurements, charger,
  and the `I2C1:` counters).

## Click wheel: `ipodwhl.dll` and `whlkeys.exe` (`src\drivers\ipodwhl`, `src\apps\whlkeys`)

- `WHL1:` reads the wheel controller the way Rockbox does, but by looking
  every millisecond instead of waiting for its interrupt: read the flags,
  write the set ones back (the only write), read the report. A controller
  whose clock is gated (Rockbox gates it while the hold switch is on) is
  left alone (this path was not tested on purpose). Reports become events (`whlcore.c`): a button down or up, the
  wheel one step on (24 steps a turn).
- `whlkeys.exe` (started with Windows) turns the events into keys: wheel =
  Down (clockwise) and Up, centre = Enter, MENU = Escape (held 0.6 s: the
  Start menu), play/pause, next and previous = the media keys. A program in
  front can ask for the wheel to be the volume keys instead
  (`WM_WHLKEYS_MODE`).
- Navigation mode (`HKLM\Software\iPod\WheelKeys`, `Navigation` = 1, the
  image's default; read when `whlkeys.exe` starts): next/previous = Tab /
  Shift+Tab, play switches the wheel between Down/Up and Right/Left
  (clockwise = Right), play held 0.6 s = `WM_CLOSE` to the window in front
  (not the desktop or taskbar; CE 7 ignores Alt+F4, tried). `Navigation` 0
  = the media keys above.
- `IOCTL_IPODWHL_GET_STATUS` gives the counters plus the finger's position
  (0 to 95) and whether the wheel is touched, without taking events.
- **Expects:** the wheel controller running as Rockbox left it.
- **Does not:** start the controller or use its interrupt.
- **Check:** start "Wheel" on the desktop a second time: what the driver and
  `whlkeys.exe` have counted.

## Restart: `restart.exe` (`src\apps\restart`)

- Asks, closes a music player if one runs, then asks Windows' power manager
  for a reset; the OAL's existing watchdog reset does the rest, and the iPod
  starts as from any reset. If the power manager refuses, it calls
  `IOCTL_HAL_REBOOT` directly.

## Panels

`wavpanel.exe` (Sound check), `dskinfo.exe` (iPod disk), `pwrinfo.exe`
(Power status): read-outs of what each driver found, for bring-up and for a
photograph when something goes wrong. Nothing in them changes anything.
