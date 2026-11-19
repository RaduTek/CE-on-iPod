# CE-on-iPod Rockbox bootloader: 32 MiB load limit and wince.ipod boot

The stock Rockbox bootloader for the iPod classic loads at most **8 MiB**, and only from
`rockbox.ipod`. A CE image with GWES, the shell, storage and audio will not fit in that, and
replacing `rockbox.ipod` means losing Rockbox. This document describes a bootloader build with two
changes, and how to build and install it:

1. The load limit is raised to **32 MiB**.
2. Holding **PLAY** at power-on loads **`/wince.ipod`** from the root of the disk instead of
   Rockbox.

## The change

The change is commit `bd75172d71` ("ipod6g bootloader: 32 MiB load limit, PLAY boots
/wince.ipod") on the `ce-on-ipod-loadsize` branch of the Rockbox checkout (`C:\dev\rockbox`),
on top of `master` at `c6593ee254`. It touches two files, both for `IPOD_6G` only. The nano 3G and nano 4G
have 32 MB of RAM and keep 8 MiB and their normal boot.

| File | Change |
|-|-|
| `bootloader/ipod-s5l87xx.c` | `MAX_LOADSIZE` is `32*1024*1024` for `IPOD_6G`. The PLAY selection and the `/wince.ipod` load path are new. |
| `firmware/target/arm/s5l8702/boot.lds` | `MAX_LOADSIZE` is `32M` for `IPOD_6G` |

The linker script places the bootloader's BSS at `DRAM_ORIG + MAX_LOADSIZE`, so both values must
match. With 32 MiB the BSS moves from 0x08800000 to 0x0A000000. The page table at the top of RAM
(0x0BFFC000) does not move.

### Boot selection

The bootloader reads the buttons once, about 0.4-0.8 s after reset. That is the same moment
holding MENU is read for the Apple firmware.

| Held at power-on | Result |
|-|-|
| PLAY | Load **`/wince.ipod`** (disk root, absolute path, no `/.rockbox` fallback) |
| MENU, SELECT+LEFT, SELECT+PLAY | Apple firmware, diagnostics or disk mode (unchanged) |
| SELECT+MENU | Reset / DFU (unchanged) |
| SELECT+RIGHT | Bootloader USB mode (unchanged) |
| Hold switch on | Apple firmware (unchanged) |
| Nothing or anything else | Rockbox, `/.rockbox/rockbox.ipod` (unchanged) |

`wince.ipod` has the same format as `rockbox.ipod`:
- an 8-byte header (BE32 `71 + sum of payload bytes`, then `"ip6g"`),
- loaded to 0x08000000,
- at most 32 MiB.

If it is missing or invalid, the bootloader prints `Can't load /wince.ipod:` with the reason (for
example `File not found`, `Bad checksum` or `File too big`). Then it does what it does for a broken
`rockbox.ipod`: it enters USB mode so a fixed file can be copied over, then stops. It does not
fall back to Rockbox.

Nothing else changes: the checksum, the file format and the jump to 0x08000000 stay the same.
Normal Rockbox (`rockbox.ipod` under 8 MiB) boots exactly as before.

To review it: `git show ce-on-ipod-loadsize` in the Rockbox checkout.

## CE side (already done in this repo)

- **`config.bib`:** `NKLEN` is 32 MB (`RAM` starts at 0x82000000). AUTOSIZE moves the boundary to
  the real image end, so this costs no RAM.
- **`mkrbipod.ps1`:**
  - Allows 32 MiB and prints a note above 8 MiB.
  - Writes only the real CE image, not the padding up to `ROMSIZE`. The end is found from
    `ROMHDR.physlast` through the `CECE` signature at offset 0x40.
  - `-NoTrim` writes the whole `nk.nb0`, as before.
- **`PostMakeimg.bat`:** writes **`wince.ipod`** in the flat release directory.

Images that stay under 8 MiB after trimming still boot with the **stock** bootloader, but only
when copied as `/.rockbox/rockbox.ipod`, which replaces Rockbox. The custom build loads them as
`/wince.ipod` next to Rockbox, and is required once an image grows past 8 MiB.

## Build environment (Windows host, WSL2)

Rockbox builds on Linux. On a Windows PC, use WSL2 with Ubuntu.

### 1. Install WSL2 and Ubuntu (once)

In an administrator PowerShell:

```
wsl --install -d Ubuntu
```

Reboot if asked, then start "Ubuntu" from the Start menu and create a user.

### 2. Install the build prerequisites (once)

In the Ubuntu shell:

```
sudo apt update
sudo apt install build-essential git perl autoconf automake libtool libtool-bin bison flex \
                 texinfo zip wget libusb-1.0-0-dev pkg-config
```

On Ubuntu, `libtool` only installs `libtoolize`. The `libtool` program that `rockboxdev.sh`
looks for is in `libtool-bin`. Without it the script stops with `"libtool" is required`.

### 3. Build the Rockbox ARM toolchain (once, about 15-30 minutes)

This uses the Rockbox script, which builds `arm-elf-eabi` binutils 2.38 and gcc 9.5.0 with the
Rockbox patches:

```
cd /mnt/c/dev/rockbox/tools
sudo RBDEV_TARGET=a ./rockboxdev.sh
```

It installs to `/usr/local` by default. To install somewhere else (no `sudo` needed), use
`RBDEV_PREFIX=$HOME/rbdev` and add `$HOME/rbdev/bin` to `PATH`.

Check it worked:

```
arm-elf-eabi-gcc --version
```

This should report 9.5.0.

If a download or step fails partway, do not just rerun the script:

- **Interrupted downloads.** A failed download can leave a truncated file in `/tmp/rbdev-dl`,
  which the next run reuses ("File already exists"). Delete it first.
- **Patch prompt on a rerun.** The script reuses an already-extracted gcc tree but applies the
  Rockbox patch again, which brings up a "Reversed (or previously applied) patch detected"
  prompt. Answer `n`, then delete the tree.
- **Restart at gcc.** binutils installs first, so a failure during gcc can restart at gcc. The
  example below switches to ftp.gnu.org because mirrors.kernel.org reset the MPFR download
  on the first setup:

```
sudo rm -rf /tmp/rbdev-build/gcc-9.5.0 /tmp/rbdev-build/build-gcc
sudo rm -f /tmp/rbdev-dl/mpfr-4.1.1.tar.xz        # whichever download failed
sudo RBDEV_TARGET=a RBDEV_RESTART=gcc GNU_MIRROR=https://ftp.gnu.org/gnu ./rockboxdev.sh
```

### 4. Build the bootloader

Build outside the source tree, in the WSL home directory, which is much faster than `/mnt/c`. The
sources are read from the Windows checkout, which must have the `ce-on-ipod-loadsize` branch
checked out:

```
mkdir -p ~/build/ipod6g-boot && cd ~/build/ipod6g-boot
/mnt/c/dev/rockbox/tools/configure --target=ipod6g --type=b
make -j$(nproc)
```

The result is `bootloader-ipod6g.ipod`.

Check that the new limit is in the binary. The bootloader BSS must start at 0x0A000000 rather
than 0x08800000:

```
arm-elf-eabi-nm bootloader.elf | grep -E ' (_edata|_end)$'
```

Both addresses should be 0x0A0xxxxx.

To keep a stock bootloader for rollback, build the unmodified one too:

```
cd /mnt/c/dev/rockbox && git switch master
mkdir -p ~/build/ipod6g-boot-stock && cd ~/build/ipod6g-boot-stock
/mnt/c/dev/rockbox/tools/configure --target=ipod6g --type=b && make -j$(nproc)
cd /mnt/c/dev/rockbox && git switch ce-on-ipod-loadsize
```

Run `git switch` from Windows if git in WSL complains about the checkout's ownership or line
endings.

Alternatively, `mks5lboot --bl-uninst ipod6g` removes the bootloader entirely.

### 5. Build mks5lboot (the installer)

```
cd /mnt/c/dev/rockbox/utils/mks5lboot
make
cp mks5lboot ~/build/
```

On Linux it uses libusb-1.0.

## Installing the bootloader

`mks5lboot` talks to the iPod's boot ROM in DFU mode. The boot ROM is always reachable (hold
SELECT+MENU), so a bad bootloader can always be replaced or removed.

### Give WSL access to the iPod (usbipd-win)

WSL2 does not see USB devices by itself. Install usbipd-win on Windows, from an administrator
PowerShell:

```
winget install usbipd
```

### Flash

1. Close iTunes, and stop `iTunesHelper.exe` if it is running.
2. Connect the iPod and put it in **DFU mode**: hold SELECT+MENU for about 12 seconds. The screen
   stays black.
3. In an administrator PowerShell, find the DFU device ("Apple Mobile Device (DFU Mode)" or
   VID 05AC), then share and attach it to WSL:
   ```
   usbipd list
   usbipd bind --busid <BUSID>
   usbipd attach --wsl --busid <BUSID> --auto-attach
   ```
   Leave this window open. `--auto-attach` re-attaches the device when the iPod re-enumerates
   during the install.
4. In Ubuntu:
   ```
   cd ~/build
   sudo ./mks5lboot --dfuscan          # expect: [INFO] DFU device state: 2
   sudo ./mks5lboot --bl-inst ipod6g-boot/bootloader-ipod6g.ipod
   ```
5. The iPod beeps:
   - 2000 Hz: the installer is running.
   - 1000 Hz + 2000 Hz double tone: installed. The iPod reboots into the new bootloader.
   - A low 330 Hz tone: install failed, and the old NOR content is kept.

   See `utils/mks5lboot/README`.

If usbipd gives trouble, the alternative is a Windows build of `mks5lboot` with the Apple Mobile
Device driver (iTunes) or a WinUSB driver from Zadig. See `utils/mks5lboot/README`, "Windows
only".

## Verify

1. **Rockbox unchanged:** power on without any key. The screen shows `Loading Rockbox...` and
   Rockbox starts as before.
2. **Missing file:** with no `/wince.ipod` on the disk, hold PLAY at power-on. The screen should
   show `Loading Windows CE...`, then `Can't load /wince.ipod:` and `File not found`, then USB
   mode.
3. **CE boots:** copy `wince.ipod` to the disk root and hold PLAY at power-on. The screen should
   show `Windows CE loaded.`, and CE should boot (KITL connects, or the serial banner appears).
4. **32 MiB limit:** build `wince.ipod` with `mkrbipod.ps1 -NoTrim`, which gives the padded
   32 MiB `nk.nb0` (exactly the limit; the bootloader rejects only larger files). It should load and boot like the trimmed one. With the
   stock bootloader, anything over 8 MiB gives `File too big`.

## Rollback

- Reinstall the stock bootloader built in step 4 with `mks5lboot --bl-inst`, or remove the
  bootloader with `mks5lboot --bl-uninst ipod6g`.
- Holding MENU at power-on always boots the Apple firmware.
