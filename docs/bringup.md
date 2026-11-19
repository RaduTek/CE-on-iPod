# Bring-up guide: ipod6g BSP

## Boot chain

1. **Boot ROM** loads the Rockbox bootloader from NOR flash into IRAM.
2. The **Rockbox bootloader** brings up clocks, SDRAM, LCD and the HDD, then mounts the FAT32 volume.
   - With the CE-on-iPod bootloader build ([bootloader.md](bootloader.md)): holding **PLAY** at
     power-on loads **`/wince.ipod`** from the disk root. There is no fallback: if it is missing
     or invalid, the bootloader shows the error and offers USB mode. Any other start loads
     Rockbox as usual.
   - With the stock bootloader: it loads `/.rockbox/rockbox.ipod` (falling back to
     `/rockbox.ipod`), so the CE image has to be copied under that name.
   - The file is an 8-byte header followed by the payload. The header is a big-endian checksum
     (`71 + sum of payload bytes`), then `"ip6g"`.
   - The payload is copied to **PA 0x08000000**: at most 8 MiB with the stock bootloader, 32 MiB
     with our build (see [bootloader.md](bootloader.md)).
   - Interrupts are masked and the caches synced, then it branches to the payload's first byte.
3. The payload is the CE image `nk.nb0`, linked at VA 0x80000000, which is PA 0x08000000.
   romimage puts a jump page at its start that branches to `StartUp` (`src/oal/oallib/startup.s`).
4. `StartUp` does the following, then calls `KernelStart` with the physical address of
   `OEMAddressTable`:
   1. Switches to SVC mode.
   2. Masks both VICs and stops the Rockbox tick.
   3. Cleans the D-cache, then turns the MMU and caches off. The map is identity, so this is safe.
   4. Prints `CE>` if early UART output is enabled.
5. The kernel turns on the MMU, then calls:
   1. `OEMInitDebugSerial`
   2. `OEMInit`, which sets up the VICs, the 1 ms tick on timer B and the perf counter on timer E,
      then starts KITL.

Holding **MENU** while powering on boots Apple's firmware instead. That is the recovery path if a
CE image hangs.

## Memory map

| PA | VA (cached / uncached) | Size | Use |
|-|-|-|-|
| 0x08000000 | 0x80000000 / 0xA0000000 | 64 MB | SDRAM. NK (image) at the start, max 32 MB (8 MB with the stock bootloader), then RAM. |
| 0x38000000 | 0x84000000 / 0xA4000000 | 128 MB | S5L8702 I/O |
| 0x22000000 | (MMU off only) | 128 KB | IRAM0. `startup.s` uses its first 32 KB as a stack. |

## Serial wiring (dock connector)

| 30-pin dock | Signal | USB-serial adapter |
|-|-|-|
| 11 | Serial GND | GND |
| 12 | iPod TX (UART0 TXD) | RX |
| 13 | iPod RX (UART0 RXD) | TX |

- Use **3.3 V** logic levels. Do not connect the adapter's VCC.
- The pin numbers are taken from accessory and iAP usage, not from the Rockbox source, so check
  them. If nothing shows up, swap TX and RX first.
- Line settings: 115200 8N1, **no flow control**. The iPod side is about 114286 baud (-0.8%),
  which is fine.

## Getting the BSP into the build VM

`C:\WINCE700\platform\ipod6g` must contain this repo's `ipod6g` folder. Either:
- clone the repo in the VM and create a junction:
  `mklink /J C:\WINCE700\platform\ipod6g <clone>\ipod6g`
- or copy the folder.

The OS design lives in `<clone>\iPodCE`. Open `iPodCE.sln` in Visual Studio 2008.

If Platform Builder doesn't list the BSP, check that `ipod6g\Catalog\ipod6g.pbcxml` is present and
refresh the Catalog Items View.

## Configurations

| Configuration | KITL | Early `CE>` | Use |
|-|-|-|-|
| ipod6g ARMV5 Release | no (`IMGNOKITL=1`) | yes | First boots. Everything goes raw to UART0. |
| ipod6g ARMV5 Debug | yes | no | KITL with full debug kernel. Over 8 MB it needs the 32 MiB bootloader. |
| ipod6g ARMV5 Checked | yes | no | KITL, retail-optimised with debug messages. |

Set `BSP_EARLYUART=1` in `ipod6g.bat` to get `CE>` in KITL builds as well.

## Build

1. Choose the configuration, then run **Build Solution** (sysgen and the BSP). The first ARMV5
   build also compiles the `platform\common` libraries for ARMV5.
2. makeimg runs `PostMakeimg.bat`, which writes `wince.ipod` next to `nk.nb0` in the flat
   release directory. Expected log lines:
   ```
   mkrbipod: CE image is NNNN bytes of 33554432 in nk.nb0 (ROMHDR physlast), trimming padding
   mkrbipod: wrote wince.ipod (NNNN byte payload, checksum 0x........, first word 0xEA......)
   ```
   - The first word should be an ARM branch (`0xEA......`). That is romimage's jump page.
   - The payload can be no larger than 32 MiB, the NK region. Above 8 MiB it needs the
     CE-on-iPod bootloader build.
3. Optional check:
   `powershell -ExecutionPolicy Bypass -File mkrbipod.ps1 -Verify -InFile wince.ipod`

makeimg runs the copies of `PreFmergeBib.bat` and `PostMakeimg.bat` in the flat release directory.
After they change in `ipod6giles`, run **Copy Files to Release Directory** before makeimg.
`PreFmergeBib.bat` deletes the previous `nk.bin`, `nk.nb0`/`nk.nb1` and `wince.ipod` before romimage
runs. romimage, and with it `PostMakeimg.bat`, therefore runs on every makeimg (makeimg skips
both while `nk.bin` is up to date), and a leftover `nk.nb1` from an earlier, larger build cannot
fail the packaging step.

If makeimg reports that the image does not fit, or `nk.nb1` exists, the image is larger than
32 MB. Images between 8 and 32 MiB need the bootloader build in [bootloader.md](bootloader.md).

## Install and run

1. Put the iPod in disk mode (or use the Rockbox bootloader's USB mode: SELECT+RIGHT at power-on).
2. Copy `wince.ipod` to the **root** of the iPod's disk and eject it. Rockbox's own files are not
   touched.
   - With the stock bootloader instead: back up `/.rockbox/rockbox.ipod`, then copy `wince.ipod`
     over it as `/.rockbox/rockbox.ipod`.
3. Connect the serial adapter and open a terminal at 115200 8N1.
4. Reboot the iPod: hold MENU+SELECT until the screen goes black, then release and immediately
   **press and hold PLAY** until `Loading Windows CE...` appears. This is the same timing as
   holding MENU for the Apple firmware: the buttons are read once, shortly after the reset.
   From power-off, hold PLAY to switch the iPod on and keep holding it. Without PLAY, Rockbox
   starts as usual.

### Expected output (Release, `IMGNOKITL=1`)

```
CE>                                   <- startup.s, before KernelStart
ipod6g: OEMInitDebugSerial (UART0 115200 8N1)
ipod6g: built <date> <time>           <- confirms the new image was loaded
...kernel messages...
OEMInit++
DCache: ... ICache: ...               <- ARM926: 16 KB I / 16 KB D, 32-byte lines expected
OALTimerInit: tick 1 ms (timer B), perf counter 1 MHz (timer E)
OEMInit--
...kernel continues (filesys, device manager)...
```

How to read failures:
- **Nothing at all:** the payload wasn't run, or the UART isn't working.
  - The bootloader shows `Windows CE loaded.` on the LCD before jumping. If you see that,
    suspect the wiring or the baud rate.
  - `Loading Rockbox...` instead means PLAY was not held at the right moment.
  - If the bootloader shows an error instead (`Can't load /wince.ipod`: file not found, bad
    checksum, file too big), check the file on the disk and the PostMakeimg log.
- **`CE>` but no banner:** `KernelStart` failed. Suspect the address table or memory map.
- **Banner but stops before `OEMInit--`:** add `Debug()` prints in `OEMInit` to bisect.
- **Hang after `OEMInit--` with no further kernel output:** the timer interrupt isn't arriving.
  Check that `OEMInterruptHandler` sees IRQ 8.

### KITL (Debug/Checked)

1. In Platform Builder: **Target > Connectivity Options**.
   - Kernel Download: **None**. The image is loaded from the iPod disk.
   - Transport: **KITL Serial**, the COM port, 115200, no flow control.
2. Attach the device first, then boot the iPod.
3. Debug messages should appear in the Output window. **Target > Target Control**, then
   `gi proc`, should list processes.

## Recovery

- Hold MENU at power-on to boot the Apple OS.
- Start without PLAY to boot Rockbox. CE only starts when PLAY is held.
- If `/wince.ipod` fails to load, the bootloader offers USB mode on the error screen. Copy a
  fixed image over, then reboot.
- Use SELECT+RIGHT at power-on for the bootloader's USB mode at any time.
