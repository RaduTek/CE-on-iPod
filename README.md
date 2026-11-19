# Windows Embedded Compact 7 on the iPod classic

A Board Support Package (BSP) for running Windows Embedded Compact 7 (CE 7) on the iPod classic
(Samsung S5L8702, ARM926EJ-S, 64 MB RAM), loaded from the hard disk by the
[Rockbox](https://www.rockbox.org/) bootloader.

**Status: bring-up.** The BSP skeleton and a sample OS design are in place. Nothing has been
verified on hardware yet.

| Part | State |
|-|-|
| Boot via the Rockbox bootloader (`rockbox.ipod`) | Written, untested |
| Debug serial on the dock connector UART | Written, untested |
| Timer tick / interrupts (VIC) | Written, untested |
| Serial KITL (Platform Builder) | Written, untested |
| Display (LCD, CPU-driven updates) | Working: test pattern and GWES init verified |
| HDD, PMU, clickwheel, audio | Not started |

## Layout

- `ipod6g/`: the BSP. Goes to `C:\WINCE700\platform\ipod6g`.
- `iPodCE/`: sample OS design (headless).
- `docs/bringup.md`: boot chain, serial wiring, how to build and test.

## Requirements

- Windows Embedded Compact 7 Platform Builder with **ARMv5** support installed.
- An iPod classic with the Rockbox bootloader installed and a FAT32 ("Windows formatted") disk.
- A 3.3 V USB serial adapter and a 30-pin dock breakout.

See [docs/bringup.md](docs/bringup.md).

## Credits

- The BSP structure comes from CEonPi, the Windows CE 7 Raspberry Pi BSP.
- Hardware knowledge comes from the Rockbox project's iPod classic port.
