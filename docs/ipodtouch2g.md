# iPod touch 2G bring-up (ipodtouch2g BSP, iPodtouchCE OS design)

Status (2026-11-28): **boots to the Standard Shell desktop on the LCD with working touch input**
(Checked, packed 12.4 MB image; taps land where the finger is). BSP for CE 7 on the iPod touch
2nd generation (MC model, iOS 3.1.3).

If an image that built fine does not start at all (no blue screen, no `CE>`, reset into iOS),
check the start-up chain first: on 2026-11-28 a good image failed several times in a row and
then booted again unchanged (state of the device or of loadibec, not the image).

## Third boot (2026-11-28, verified): packed image, display, desktop
- Packed image unpacked and started: `image len 0x015eacb8`, `packed len 0x00bbb959`.
- RAM: section 0x81616000 + 0x065c6000 = 0x87BDC000, 26054 pages (101 MB); no "adjusted" line.
- iptlcd: `window 0: format 0x00200700, framebuffer 0x0fc00000, line 320, size 320x480`,
  colour bars shown, `GWES initialized properly`, desktop drawn (37 blts, 40 lines in 5 s).
- **The hand-over state varies between boots**: this time MMU and caches off
  (`sctlr 0x0005007a`), CPSR 0xd3, VICs off and the tick counter **stopped**
  (`timer ctrl 0x0001801a`, bits 1 and 3 set = off as in openiBoot; `ticks/100ch 0`). The first
  boot had the MMU on and the counter running. startup.s and OALTimerInit handle both; the
  "tick counter stopped, starting it" message is real, not a false alarm.
- Lines that end with LF only (NDIS, RDR, GDI messages) left the terminal at the old column;
  OEMWriteDebugByte now adds the CR (not yet tested).
- Expected: `Could not find touch driver dll` (no touch yet), `CAudPlay ... waveOutOpen failed`
  (no audio driver).

## Second boot (2026-11-28, verified): UART fix works
- The log is readable after `OEMInit--`; `OALTimerTickIsr: timer status 0x01000000` three times.
- `KernelFindMemory: MainMemoryEndAddress adjusted from 8fc00000 to 88000000`: config.bib had RAM
  up to VA 0x8FC00000, but 128 MB at 0x80000000 end at 0x88000000 (the reserved top 4 MB start
  at VA **0x87C00000**). The kernel's RAM probe trimmed it, so CE's RAM included the framebuffer.
  Fixed: `RAMLEN` 0x05C00000 (RAM 0x82000000-0x87BFFFFF). Expected next: no "adjusted" line.
- `OALTimerInit: tick counter stopped, starting it`: the counter really was stopped at hand-over
  (see the third boot); the restart (openiBoot's sequence) works.

## Upload limit and packed images (2026-11-28)
- iBSS receives the upload in a **16 MiB USB buffer**. Tested with cut-down images: a 12 MiB
  payload starts, 16 MiB resets into iOS (the user found the buffer size). The Checked image with
  GWES and the Standard Shell is 23 MB.
- `mkimg3.ps1 -Compress` (default in PostMakeimg.bat; `BSP_NOCOMPRESS=1` turns it off) keeps the
  first 64 KB as they are (jump page, startup.s code and strings; startup code is at 0x1000-0x1E00)
  and packs the rest in LZ4 block format. Pack header in the jump page (romimage leaves
  0x04-0x3F zero): 0x20 `LZCE`, 0x24 packed length, 0x28 head length, 0x2C unpacked length.
  The script unpacks its own output and compares it before writing. Checked image: 22981676 ->
  12369116 bytes (53.8%), 4 s on the host. An IMG3 over 16 MiB fails the build.
- startup.s: copies the packed data to PA 0x0E000000, then the stub (at 0x0FBF0000) copies the
  head to 0x08000000, unpacks the rest behind it and clears the pack header. Byte loops with the
  D-cache off (MMU off): expect a few seconds between the blue and the green screen.
  The probe prints `packed len` too; a bad header prints `CE> bad pack header, stopped`.

## Display driver (src\drivers\iptlcd, iptlcd.dll) - verified 2026-11-28 (desktop shown)
- GPE flat driver, loaded by GWES in kernel mode. The mode comes from window 0 as iBoot left it
  (320x480 RGB888, framebuffer 0x0FC00000, 320 pixels per line); 16 bpp also handled.
- GDI draws straight into the framebuffer through the kernel's cached view (VA 0x87C00000);
  after every blit and line the touched rows are written back with `CacheRangeFlush`
  (CEonPi vc4disp method). No flush thread, no shadow buffer.
- The driver refuses a framebuffer outside the reserved top 4 MB.
- Bring-up: colour bars 1.5 s at SetMode (`BSP_LCD_NOTESTPATTERN=1` turns them off), the first
  4 blits and a status line 5 s after SetMode are logged.
- OS design: `sysgen_minwmgr`, `sysgen_display`, `sysgen_standardshell`, commctrl, commdlg, ctlpnl.
  No input yet, so the desktop is view-only.

## Touch (hardware identified 2026-11-28; probe steps 1 and 2)
openiBoot has **no S5L8720 touch support**; its Zephyr2 driver (`plat-s5l8900/multitouch-z2.c`,
iPhone 3G / iPod touch 1G) is the protocol reference. The touch 2G facts come from an iOS ioreg
dump of an N72 (gist by munnerz, copy in `ipodtouch2g\files\z2\`, gitignored). Not verified on
our device:

| | ioreg (iOS) | openiBoot S5L8720 |
|---|---|---|
| Driver / protocol | `AppleMultitouchZ2SPI`, `multi-touch,n72`, bootload packet type "Z2" | Z2 code (S5L8900) |
| SPI bus | `spi4@6100000` = 0x3E100000, VIC IRQ 0x37 | `SPI4 0x3E100000`, `SPI4_IRQ 0x37` |
| Chip select | GPIO pin 0x0a07 (`function-spi_cs0`) | `GPIO_SPI4_CS0 0xA07` |
| Reset | GPIO pin 0x0a06 (`function-reset`, flags 01 01 01 00) | - |
| ATN interrupt | GPIO interrupt 0x6D (`interrupts`, parent = gpio@4F00000) | GPIO interrupt block 0x39700000 |
| Power | power chip LDO 3 at 1800 mV (`function-power_ldo` = "Lump" 03 02 0x0708) | - |
| Power chip | `pmu@73` on i2c0, Dialog D1759 (`pmu,d1759`) | I2C bus 0, 0xE6/0xE7 (= 0x73), no LDO code |
| SPI timing | `reg`: cs 0, 0xa6, 01 01 01 08, 5000, 10000 | Z2 "normal speed" {83000 Hz, 5000, 10000} |
| SPI clock gate | `clock-gates` 0x26 | `SPI4_CLOCKGATE 0x12` (different numbering?) |

Expected answers once the firmware runs (from ioreg): Family ID 83, 15 rows x 10 columns,
bcdVersion 81, Endianness 1, max packet size 660, surface 5000 x 7500.

Firmware and calibration (Apple's; extracted from the dump, kept in `ipodtouch2g\files\z2\`,
never committed): `z2fw-0x0051.bin` (48612 bytes, "Constructed Firmware", version 0x0051),
`z2cal.bin` (256 bytes, `multi-touch-calibration`), `z2proxcal.bin` (128 bytes, all zero).

openiBoot's start-up sequence: ATN interrupt on, power on, reset pulse (low 200 ms, high, 15 ms),
wait for ATN, send the constructed firmware at fast speed, expect ATN + ack 0x4BC1; then the
proximity calibration, calibration, calibrate, execute, interface version, info reports.

### n72-linux (found 2026-11-28)
[n72-linux](https://github.com/n72-linux/n72-linux) runs Linux 6.14 on the N72 with working
touch (`drivers/input/touchscreen/zephyr2.c`), D1759 power chip (`drivers/mfd/apple-n72-pmu-core.c`,
`drivers/regulator/apple-n72-pmu-regulator.c`) and device tree
(`arch/arm/boot/dts/samsung/s5l8720-n72.dts`) in [n72-linux/linux](https://github.com/n72-linux/linux),
branch `n72`. GPL: documentation only, as with Rockbox. Facts taken from it:
- D1759 LDOs: LDO n voltage in register 0x19 + n, bits 0-4, (mV - base) / step: LDO 0 2000/50,
  LDO 1-4 and 8 1200/100, LDO 5-7 2500/50. Enable: register 0x10 bit n + 2 (n < 6), 0x11 bit
  n - 6 (6-8), 0x13 bit 2 (LDO 9). Rails: LDO 3 touch 1.8 V, LDO 4 LCD 3.0 V, LDO 2 TV out,
  LDO 5/9 dock accessory. Core: DOWN0 = 0x14, (mV - 725) / 25.
  **Our dump (iBSS start): 0x10 = 0x7f (LDO 3 bit 5 on), 0x1c = 0x06 (1.8 V): touch powered.**
  The DT notes LDO 3 can start off on some cold boots.
- Touch: reset GPIO 86 = 0x0a06 **active-high** (high = held in reset; our probe found it
  high), CS GPIO 87 = 0x0a07 active-low, data ready GPIO interrupt 0x6d **level-low**; GPIO
  interrupt = pin + 24, so data ready is pin 85 = **0x0a05** (an input, as the probe shows).
  SPI mode 3 (cpol, cpha), 10 us CS setup/hold, 83 kHz normal, 4.5 MHz firmware upload
  (2 MHz frame reads). SPI4 = VIC1 23 (= 0x37), clock gates 2/4 and 4/20.
- Boot (zephyr2.c z2_boot, up to 4 tries): reset on 200 ms, a 16-byte dummy transfer
  (1a a1 18 e1 x7) so SCLK idles high, reset off, 15 ms, the dummy transfer again to take the
  "HBPP ready banner (0x1f01)", firmware, calibration, calibrate, execute, interface version.
- Firmware file: iOS `/usr/share/firmware/multitouch/iPod.mtprops`, key `Z2F53,1`
  (ours comes from the ioreg "Constructed Firmware"; calibration from `multi-touch-calibration`).
- RAM: PA 0x08000000-0x08000FFF is reserved in their DT (LLB wake stub, sleep token at +0x90).
  Our image overwrites it; only matters once we implement suspend through LLB.
- PMU interrupt: GPIO interrupt 0x61 (pin 73 = port 9 bit 1), level-low.

Steps, each testable on its own:
1. Probe in the OAL or a small driver: dump the power chip registers over I2C0, the GPIO state
   of pins 0x0a06/0x0a07 and the SPI4 registers as iBoot left them. **Done 2026-11-28**
   (`src\oal\oallib\touchprobe.c`, lines start with `touchprobe:`, `BSP_NOTOUCHPROBE=1` leaves
   it out): all blocks answered; reset 0x0a06 output high (in reset), CS output low, data ready
   input low, GPIO interrupts all 0, SPI4 setup 0x8 (master) pin 3 clkdiv 1, LDO 3 on.
2. Reset + data ready: **done 2026-11-28** (same file, now behind `TOUCHPROBE_STEP2`). Pins
   driven through the FSEL register (openiBoot gpio_custom_io: 0xE low, 0xF high) work: reset
   read back 1 / 0 / 1. Both runs (with and without SPI4 mode 3): data ready low in reset, high
   10 us after release, low again from ~35 us on (500 ms watched). The GPIO interrupt status
   never set (not configured). Not yet proof that the controller answers (the line was low in
   reset too): that needs an SPI read.
3. SPI link to the bootloader, no firmware: **verified 2026-11-28** (`Step3SpiLink`).
   Dummy transfer in reset: all 00, data ready 0. 15 ms after reset off: data ready 0 (banner
   waiting). Second dummy transfer: **`1f 01`** then `48 79` x7, data ready **1** afterwards.
   Short acks (1a a1): `48 79` each time (0x4879 = bootloader idle; openiBoot's 3G code waits
   for 0xffff, Linux uploads without waiting). No SPI timeouts, status 0x00400833. So the
   controller is powered, leaves reset and talks; the SPI4 set-up below works.
   Set-up used: SPI4 as n72-linux spi-s5l8720.c: SETUP 0x401e (fixed clock, master, CPOL,
   CPHA, 8 bit), PIN 2, CLKDIV 290 (82.8 kHz), polled 16-word FIFOs, CS GPIO low 10 us around a
   transfer, 5 ms after a send, 10 ms after an answer. z2_boot sequence: reset on 200 ms, dummy
   transfer (1a a1 18 e1 x7), reset off, 15 ms, dummy transfer again (the ready banner), then 4
   short acks (1a a1). Logs every byte received and the data-ready level after each transfer.
   The firmware upload cannot run in OEMInit (no file system yet); it goes into the touch driver.
3. Firmware upload over SPI4, expect ack 0x4BC1, then read the family ID (83).
4a. Driver that starts the controller and logs touches. **First run 2026-11-28: start-up
   verified**: banner `1f 01`, firmware 48612 bytes in 117 ms (polled; 12 ms of SPI at 4 MHz)
   ack 0x4bc1, calibration ack 0x4bc1, Zephyr2 version 0x5a02002a (no register writes needed),
   calibrate ack 0x48794879, interface version 1, max packet 660, family 0x53, 15 x 10, bcd
   0x0051 (so flip-nop / 0xeb frames), dimensions report 88 13 00 00 4c 1d 00 00 (surface
   4602 x 7306). **Frames: every read fails after the first touch** (0 good frames, errors
   counting up every ~3 ms, data ready stays low). Second version: logs the failed check and
   the bytes of the first 10 bad frames, reads one frame right after start-up (as Linux),
   restarts the controller after 100 bad frames in a row (3 times at most).
   **Second run: received bytes one position late.** First read after start-up: empty, OK.
   On a touch: length query OK (len 54), then the 59-byte frame read received
   `00 ea 02 36 00 de 44 ...`: without the leading byte a valid frame (`ea`, nop 02, payload
   0x36, header sum 0x200). Every later transfer stayed one byte late (leading `00` or `02`).
   Likely a byte left in the SPI receive FIFO. Third version: z2hw.c empties the receive FIFO
   before each transfer and after it (waits 200 us for a byte beyond the length) and counts
   both; the counts are logged with bad frames and in the 30 s statistics.
   **Third run: FIFO counts 0**, so the slip is the controller's: its answers are one byte
   stream (16-byte length reply, then len + 5 frame bytes); now and then, while a finger is
   down, a transfer starts with a filler `00` and the answer follows one byte late, its last
   byte still waiting in the controller. **Fourth run** (one-byte realign, answer dropped): the
   first touch frame read fine (`sensor 513,357`), the second came late, and dropping it left
   the controller mid-stream (a "length query" then received frame bytes `ea 01 36 ...`).
   Fifth version: a late answer is completed instead (read the one waiting byte, drop the
   filler, check it as usual: `realigns` counts them); frames read at registry `FrameHz`,
   default 500 kHz (filler bytes look like the controller not keeping up at 2 MHz); a restart
   logs its first bad frames in detail again. Not yet run.
   `src\drivers\ipttouch` (`ipttouch.dll`, stream driver `ZTP1:`, device manager, kernel
   mode): `z2hw.c` = SPI4/pins/delays as step 3, `z2.c` = the protocol written from n72-linux's
   zephyr2.c, `ipttouch.c` = worker thread. Loads `\Windows\z2fw.bin` and `\Windows\z2cal.bin`
   (PreFmergeBib.bat copies them from `files\z2\` into the release directory; **the image then
   contains Apple's firmware: do not publish wince.img3**). Logs: banner, firmware ack (expect
   0x4bc1), calibration acks, Zephyr2 version, interface version / max packet, the info reports,
   then touch down / move (every 250 ms) / up in sensor units and screen pixels. Polls data
   ready every 2 ms (no interrupt yet). `BSP_NOIPTTOUCH=1` leaves it out; the OAL probe is now
   off by default (`BSP_NOTOUCHPROBE=1` in ipodtouch2g.bat).
   **Sixth run (2026-11-28): works.** 0 errors; every frame read is one byte late
   ("late answers completed" = frames - 1), completed every time. Double taps near the corners
   clockwise from top left and the middle: screen 34,14 / 303,26 / 306,467 / 15,473 /
   157,258: x to the right, y downwards, no swap or flip; the dimensions-report surface
   (4602 x 7306) maps the corners to within ~15 px of the edges.
4b. Touches as input: **verified 2026-11-28: touch works in Windows, taps land correctly**
   (the quarter-pixel, CALIBRATED reporting and the identity CalibrationData are right; no
   calibration screen). ipttouch.dll is now the PDD of
   Microsoft's CE 7 touch stream driver (`tchstreammdd.lib`, exports `tchstream.def`,
   `TchPdd_Init` + deinit/ioctl/powerdown/powerup, as SMDK6410): device manager loads it as
   `TCH1:` from `Drivers\BuiltIn\Touch` (Flags 8, IClass touch driver class + power-manageable
   touch), tchproxy.dll (in GWES) finds it by the class. Single touch: the first finger of each
   frame, DOWN|INRANGE / MOVE|INRANGE / UP, always PRIMARY|CALIBRATED, x/y in quarter pixels
   (tchproxy SUBPIXEL_ACCURACY 4; tchproxy skips calibration for CALIBRATED samples, see
   tchproxy.c). `CalibrationData` = identity for tchproxy's default points on 320 x 480, so no
   calibration is asked for. TCH_PROPS: 60 Hz, range 0-1279 x 0-1919, single touch. Needs the
   Touch Screen (Stylus) catalog item (SYSGEN_TOUCH) and BSP_NOTOUCH unset (common.reg/bib add
   tchproxy/tchcaldll). Logs only down/up positions and a 60 s statistics line.


## First boot results (2026-11-28, verified on the device)
- loadibec needs the **iTunes USB driver**, not the Windows Update one: with the latter the
  uploads after iBSS fail (`Unable to upload iBSS payload`, then `error -6` =
  `IRECV_E_USB_UPLOAD` for wince.img3).
- iBoot copies the IMG3 file to **0x09000000** and runs the DATA payload in place: entry PC
  0x09001040, image at 0x09000040 (DATA payload offset 0x40). startup.s moved it to 0x08000000.
- Hand-over: SVC mode, FIQ masked, IRQ not masked (CPSR 0x60000053); MMU, D-cache, I-cache,
  branch prediction and the U bit on (SCTLR 0x0045187d); page table in SRAM (TTBR0 0x2202c000).
- CPU ID 0x410fb764 (ARM1176 r0p4); 16 KB I and D cache, 4 ways, 32-byte lines. VFP present
  (FPSID 0x410120b5). Peripheral port remap 0x38000012 (0x38000000, 128 MB).
- CHIPINFO 0x87200047: security epoch 1, so base clock 24 MHz and tick counter 12 MHz.
  Measured: `ticks/100ch` 0x198ce = 104654 counts in about 101 characters at 115200 baud, i.e.
  about 11.9 MHz. **12 MHz verified.**
- UART0 is the dock UART at 115200 8N1: UCON 0x5405 (clock select 1, 24 MHz), UBAUD 0x0c
  (24 MHz / 16 / 13 = 115385 baud), FIFOs off (UFCON 0).
- Display window 0: RGB888 (format 0x00200700), framebuffer **0x0FC00000**, 320 pixels per line,
  320x480 (size 0x014001e0): as openiBoot.
- Timer 4 interrupt: status 0x01000000 (timer 4 bits 0x01) at each tick.
- Garbled UART after `OEMInit--`: every other character lost and the others doubled
  ("ooaa  eeooyy" = odd characters of "Total Memory"). The kernel applies the cache settings after
  OEMInit; with the faster CPU, the Tx-empty status still read "empty" right after a write, so
  the next byte overwrote it. Fix: after writing UTXH, wait until Tx-empty drops
  (`S5lUart0PutByte`, also `EarlyPutc`).

## Hardware (from openiBoot `plat-s5l8720/`, unverified)
- Samsung S5L8720, ARM1176JZF-S (ARMv6), 128 MB DRAM at PA 0x08000000-0x0FFFFFFF.
- Same peripheral addresses as the S5L8702 for UART0 (0x3CC00000), VIC0/VIC1 (0x38E00000/1000),
  timers (0x3C700000) and watchdog (0x3C800000). Differences: UART speed register `UBAUD` (0x28)
  instead of `UBRDIV`/`UBRCON`, a 64-bit tick counter at 0x3C700080/84, timers on IRQ 7,
  clock gates in five registers (0x3C500048, 4C, 58, 68, 6C).
- Display controller at 0x38900000, window 0: format 0x20 (bits 11:8, 7 = RGB888, 4 = RGB565),
  framebuffer address 0x24, line length 0x28, size 0x30. openiBoot's framebuffer is at 0x0FC00000;
  panel "n72", 320x480.
- openiBoot's S5L8720 port is incomplete (no NAND, its UART clock gate number is outside its own
  table), so its values are hints, not facts.

## Boot chain
The MC model has the new bootrom: no lasting bootloader patch, every cold boot is tethered.
DFU mode -> bootrom exploit -> patched iBSS/iBEC (recovery mode) -> `loadibec wince.img3`.
The patched iBoot starts the IMG3 payload at its first byte (romimage's relative branch to
`StartUp`). The load address is unknown; `startup.s` copies the image to its link address
(PA 0x08000000) when it runs anywhere else.

`PostMakeimg.bat` writes into the release directory:
- `wince.img3`: IMG3 of type iBEC. With `ipodtouch2g\files\template-ipt2g.img3` present (copy of
  openiBoot's `mk8900image\template-ipt2g.img3`, not in git), all of its tags are kept and only
  DATA is replaced, as openiBoot's own build does. Without it, a minimal IMG3 (TYPE, DATA).
- `wince.bin`: the same payload without a container (for loaders that want raw code).

Check a file with `powershell -File files\mkimg3.ps1 -Verify -InFile wince.img3`.

## Build (in the VM)
1. Make `ipodtouch2g` appear as `C:\WINCE700\platform\ipodtouch2g` (as for ipod6g) and copy
   `template-ipt2g.img3` into its `files` folder.
2. Open `iPodtouchCE\iPodtouchCE.sln`. Configurations: **Release** (IMGNOKITL=1: raw UART output and
   the hand-over probe), Checked and Debug (serial KITL).
3. Build Release first. Expected: 0 BSP errors; `mkimg3: wrote wince.img3 (...)` at the end.
   ARMV6 is installed (the ARM1136 cache library has ARMV6 objects).

## What to expect on the device (Release)
Screen colours (`BSP_NOSCREENMARKS=1` turns them off); each only appears if iBoot's framebuffer is
in the reserved top 4 MB of DRAM (0x0FC00000 and up) and window 0 is RGB888 or RGB565:

| Colour | Meaning |
|--------|---------|
| blue   | `StartUp` runs: caches and MMU are off |
| green  | the image is at PA 0x08000000, `KernelStart` is next |
| white  | `OEMInit` runs: the kernel came up with the MMU on |

Serial (dock UART, at the speed iBoot set up, 115200 8N1 assumed):
```
CE> iPod touch 2G hand-over probe (ipodtouch2g startup.s)
entry pc    0x........      where iBoot started us
cpsr / sctlr / ttbr0        mode, MMU/cache state at entry
...                         CPU ID, cache type, VFP test, clock, UART, display, timer registers
ticks/100ch 0x........      tick counter counts during 100 characters (~8.7 ms at 115200)
image at / image len / link addr
CE> KernelStart
ipodtouch2g: OEMInitDebugSerial (UART0, iBoot's settings)
OEMInit++ ... OALTimerInit: tick 1 ms ... OALTimerTickIsr: timer status ...
```
Please send the whole log: the probe answers most of the open questions below.

If the screen turns blue but nothing comes out of the UART, iBoot probably did not set the UART
up. Things to try (none verified): iBoot's `debug-uarts` setting (`setenv debug-uarts 1`,
`saveenv` from recovery mode) and the dock wiring (classic: pin 11 GND, 12 TX, 13 RX; some iPhone
serial cables also put a resistor on pin 21).

## Open questions (answered by the first boot)
- Load address and MMU/cache state at hand-over (probe: `entry pc`, `sctlr`).
- Is UART0 on the dock connector, at 115200?
- Tick counter rate: `S5lTicksPerSec` assumes base clock / 2 = 12 MHz (security epoch 1). Check:
  `ticks/100ch` x 115.2 = counts per second; `chipinfo` bit 0 = epoch.
- Timer 4 interrupt status bits (first `OALTimerTickIsr` lines).
- VFP present (`cpacr+vfp` has bits 23:20 set; OEMInit logs `VFP: present`).
- iBoot's framebuffer address and format (`clcd w0 ...`), for the display driver.
- Upload size limit of the patched iBoot (the headless Release image should be a few MB).

## Design notes
- OAL copied from ipod6g; ARMv6 parts as in CEonPi (`PROCESSOR_ARM11`, `VfpOemInit`; VFP is
  emulated if the coprocessor does not answer). Cache library: Microsoft's `oal_cache_arm1136.lib`
  (CEonPi uses a copy of the Samsung S3C6410 library; fallback if this one misbehaves).
- OEMAddressTable maps DRAM at 0x80000000 and the I/O block at 0x88000000 (uncached 0xA8000000),
  as the Microsoft SMDK6410 (ARM1176) BSP maps its I/O.
- The UART keeps iBoot's clock and speed (s5l8720_uart.h); output is dropped after a timeout if
  the UART never drains, so a dead UART cannot hang the boot.
- Perf counter = the 64-bit tick counter; system tick = timer 4 (openiBoot's event timer).
- `IOCTL_HAL_REBOOT` resets through the watchdog; the iPod then boots iOS.

## Next
1. Checked with serial KITL to Platform Builder.
2. Touch: data-ready by GPIO interrupt instead of the 2 ms poll; power down with the screen;
   maybe multi-touch (the controller reports up to 10 fingers).
3. Buttons (GPIO: home, power, volume), backlight and battery through the D1759.
4. NAND (Apple FTL), suspend through LLB (PA 0x08000000-0x08000FFF must then stay free).
