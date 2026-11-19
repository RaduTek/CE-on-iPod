# Rockbox bootloader patch

`0001-ipod6g-bootloader-32-MiB-load-limit-PLAY-boots-wince.patch` changes the Rockbox
bootloader for the iPod classic (`ipod6g`) so it can boot this Windows CE port next to Rockbox:

- **Load limit:** raised from 8 MiB to **32 MiB**.
- **Boot selection:** holding **PLAY** at power-on loads **`/wince.ipod`** from the disk root.
  Everything else boots as before.

See [../docs/bootloader.md](../docs/bootloader.md) for the details, the build environment and
flashing.

It was created with `git format-patch` and is based on Rockbox `master` at `c6593ee254`
("erosqnative: remove last EROSQN_VER usage"). The `base-commit:` line in the patch records this.
It only touches `bootloader/ipod-s5l87xx.c` and `firmware/target/arm/s5l8702/boot.lds`.

The patch modifies Rockbox code and is under the same license as Rockbox, GNU GPL version 2 or
later. That is independent of the rest of this repository.

## Apply

In a Rockbox checkout (`git clone https://github.com/Rockbox/rockbox`, the remote the patch was
made against):

```
git switch -c ce-on-ipod-loadsize c6593ee254   # or a newer master, see below
git am /path/to/CE-on-iPod/rockbox/0001-ipod6g-bootloader-32-MiB-load-limit-PLAY-boots-wince.patch
```

On a newer `master`, check that it still applies first:

```
git apply --check /path/to/0001-*.patch
```

If it doesn't, the bootloader's `MAX_LOADSIZE` define or the image-loading code in `main()` has
moved. Redo the two edits by hand, using the patch as the reference.

The patch is stored byte for byte (`*.patch -text` in `.gitattributes`), keeping the LF line
endings Rockbox uses. Do not convert it to CRLF.

## Regenerate

After changing the commit on the Rockbox branch:

```
cd C:\dev\rockbox
git format-patch -1 ce-on-ipod-loadsize --base=<master commit it is based on> -o <CE-on-iPod>\rockbox
```
