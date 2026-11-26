# Windows Embedded Compact 7 on the iPod classic

A Board Support Package (BSP) for running Windows Embedded Compact 7 (CE 7) on the iPod classic
(Samsung S5L8702, ARM926EJ-S, 64 MB RAM), loaded from the hard disk into memory by a modified
[Rockbox](https://www.rockbox.org/) bootloader.

<img src="docs/images/booted.png" height=300 />

[Boot demo video (YouTube)](https://youtu.be/UabXwbNv9Gw)

## Layout

- `ipod6g/`: the BSP. Goes to `C:\WINCE700\platform\ipod6g`.
- `iPodCE/`: sample OS design (headless).
- `docs/bringup.md`: boot chain, serial wiring, how to build and test.

## Requirements

- Windows Embedded Compact 7 Platform Builder with **ARMv5** support installed.
- An iPod classic with the patched Rockbox bootloader installed and a FAT32 ("Windows formatted") disk.
- A 3.3 V USB serial adapter and a 30-pin dock breakout.

See [docs/bringup.md](docs/bringup.md).

## Hardware Support

### Current

These are hardware features that are functional

| Feature | Driver State | Comments |
|-|-|-|
| UART | Basic | Dock connector serial (UART0), 115200 8N1, no flow control. Used for KITL debugger connection with Visual Studio |
| Display | Basic | 320x240 RGB565 only. Screen updates are pushed to the LCD by the CPU, no DMA or acceleration whatsoever |
| Audio output | Basic | I2S + DMA into the codec, at the sample rate Rockbox left it at. Needs Rockbox to have run first (Windows CE started from Rockbox); not tried from the bootloader |
| ATA HDD | Read only (writes opt-in) | PATA (not CE-ATA), UDMA with interrupt, 6.7-9 MB/s reads through the file system. Mounted as \iPod. Writing is off unless registry `AllowWrite` is 1 (write test passes: 4 MB written, flushed, read back) |
| Battery | Basic | Level and charging through the PMU, taskbar icon |
| Real-time clock | Basic | Time read from the PMU at start-up |
| Clickwheel | Basic | Wheel and buttons as keys. Needs the wheel controller running as Rockbox leaves it |

### Future

**No promises that this project will be continued, but ideally I'd like to get the following implemented:**

| Feature | Comments |
|-|-|
| ATA HDD | Writing; CE-ATA models |
| Starting from the bootloader | Codec and click wheel set-up of their own, for the PLAY start where Rockbox never runs |
| USB Function | Let the iPod be connected as a USB device to a computer (for Mass Storage) |

## Notes

### Bootloader

By default, the Rockbox bootloader can only load boot images up to 8 MB in size. A basic CE kernel image will just about fit, but if you want any sort of graphics the kernel image will exceed the maximum size and the original bootloader won't load it anymore.

A patch for the Rockbox bootloader which adds support for loading a Windows CE image `wince.ipod` of up to 32 MB from the root of the FAT32 drive when holding down the `Play/Pause` key at the bootloader is available in the [rockbox](rockbox) folder.

### Shell

Building an image with the entire CE shell is quite wasteful in terms of memory, as the Explorer based shell requires the whole network stack, which on the iPod is pointless.

Without the shell, the image is ~8 MB with everything required to run graphical apps (GWES). With the shell, the image grows to ~23 MB and takes much longer to boot, especially when connected to a debugger via KITL.

Currently, the sample VS project [iPodCE](iPodCE) has the shell catalog items selected, but I will remove them later for a lighter image.

## Credits

- The BSP structure comes from [CEonPi](https://github.com/RaduTek/CEonPi), the Windows CE 7 Raspberry Pi BSP.
- Hardware knowledge comes from the Rockbox project's iPod classic port.
