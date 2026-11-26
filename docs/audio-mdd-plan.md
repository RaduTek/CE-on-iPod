# Plan: port the sound driver to Microsoft's wave MDD + our PDD

Status: **step 2 done and verified** (2026-11-26) from ROLO and from a cold start: `wavepdd.cpp`
on `wavemdd.lib`, page engine and `getms.ps1` removed, status IOCTL version 3, `Priority256` 110,
`TestTone` 0. Start-up sound, Sound check and Windows' volume control work; no underruns or DMA
errors logged. Both starts log `DMA controller 0 quieted (pending on idle channels: 0x10)`, so the
bootloader leaves channel 4 pending too (cold: `I2SCLKDIV was 0`). Every sound is a new
`ipodwav: open 44100 Hz 16-bit stereo` (the mixer closes the device when idle; ~100 in a session
with volume changes). Step 3 (volume on the codec, `CodecVolume` 1) **verified** (cold start, 2026-11-26): 96 volume
changes through all 6 control-panel positions, steps audible, no `headphone level write failed`,
no underruns. **The plan is complete.**
First step 2 image **stopped the boot** after `ipodwav: wave MDD + PDD ... ready` (ROLO and cold):
Rockbox's LCD DMA channel 4 (also in the bootloader) is left with a pending TC and its ITC enable
on, which holds IRQ 16 high; the MDD's `InterruptInitialize` enabled it, its IST got `IGNORE`, and
`InterruptDone` let it straight back in: an endless storm at priority 110. (In step 1 the tone's
ring start happened to clear it.) Fix: `IpodPcm_Init` turns off every other DMAC0 channel's
interrupt enables and clears idle channels' pending status before the MDD enables IRQ 16; the PDD
does the same on any interrupt while nothing plays (status `spurious`).
Step 1 (commit `2bd1511`) **verified on hardware from ROLO** (cold start not yet tried). Log:
`test tone done: 168 DMA interrupts in 1004 ms (expect ~172 in ~1000 ms), longest gap 36 ms,
1 late, 0 empty, 0 timeouts, 0 DMA errors, 0 other channels`; tones correct (440 Hz left, 880 Hz
right), desktop audio still works. The 36 ms gap (and the one late ack, which covered the blocks
missing from 172) is the `[tone]` register dump: ~400 characters of synchronous RETAILMSG at
~114 kbaud. Ring start logged `cleared old interrupts of idle channels 0x10` = channel 4,
Rockbox's LCD DMA (`lcd-s5l8702.c`, `DMAC_CH_PRIO(4)`), so the stale-status clear is needed
after ROLO. Branch `shared-i2c` has the
current working driver (commit `3890175`: codec set-up on every start, verified from ROLO and cold
start).

Step 1 as written: `SYSINTR_AUDIO` (= 17) <-> IRQ 16 in `bsp_cfg.h`/`intr.c`; the ring engine
(`pcmring.h` interface) lives in `ipodpcm.c` beside the old two-page engine (same channel, never
both at once); the test tone is `pcmtone.c` (integer sine table, no float per sample), called at
the end of `IpodPcm_Init` when `TestTone` = 1. Ring start clears stale TC/error status of *idle*
DMAC0 channels (Rockbox's LCD DMA also uses DMAC0; a leftover bit would hold IRQ 16 high).
Step 2 moves the `PcmTone_Check` call into `PDD_AudioInitialize` and deletes the page engine.

## Goal

Replace `ipodwav.dll`'s upper half (Microsoft's `wavedev2\virtual` sample, copied and patched at
build time by `src\drivers\wavedev\getms.ps1`) with Microsoft's **prebuilt wave MDD
(`wavemdd.lib`)** plus our own PDD, the same model as the CEonPi analog audio driver
(`Y:\CEonPi\raspi2\src\drivers\pwmaudio`, documented in `Y:\CEonPi\docs\audio.md`). No Microsoft
source is copied into the build any more.

## Current driver vs the target

| | Now (`ipodwav`) | Target (CEonPi model) |
|---|---|---|
| Upper half | `wavedev2\virtual` sample, copied + patched by `getms.ps1` (run by `ipod6g.bat`) | prebuilt `wavemdd.lib`, linked from the public tree |
| Our code | `ipodpcm.c` (hardware) | `wavepdd.cpp` (PDD) + hardware file (ring engine) |
| Timing | driver thread polls the DMA position every 2 ms, no interrupt | MDD's IST waits on a hardware interrupt (`InterruptInitialize(gIntrAudio, ...)`) |
| Mixing | wavedev2's own mixer + SRC | CE 7's software mixer in `waveapi.dll` (swmixer); CEonPi verified it opens the device at 44100 Hz 16-bit stereo and recycles a few small buffers |

Checked on `Z:` (2026-11-26):
- `wavemdd.lib` exists for ARMV5: `Z:\public\common\oak\lib\armv5\<retail|checked|debug>\wavemdd.lib`.
- MDD source for reference: `Z:\public\common\oak\drivers\wavedev\mdd` (`wavemdd.cpp`, `wav.cpp`, ...).
- PDD interface: `Z:\public\common\oak\inc\waveddsi.h` (`gIntrAudio`, `PDD_AudioInitialize`,
  `PDD_AudioDeinitialize`, `PDD_AudioPowerHandler`, `PDD_AudioGetInterruptType`,
  `PDD_AudioMessage`, `PDD_WaveProc`; optional hooks `pfn_PDD_IoControl`, `pfn_PDD_MixerMessage`).
- `wav.cpp` passes unknown IOCTLs to `pfn_PDD_IoControl` (set it in `PDD_AudioInitialize`), so the
  Sound check panel's `IOCTL_IPODPCM_GET_STATUS` keeps working.
- The MDD reads `Priority256` from the driver's registry key for its IST
  (`GetInterruptThreadPriority`); `PDD_AudioInitialize` gets the active key path as `dwIndex`.

## Work

### 1. OAL: an audio interrupt (new)

- `src\inc\bsp_cfg.h`: `SYSINTR_AUDIO` = `SYSINTR_FIRMWARE+1`; bump `SYSINTR_ENDOFSTATIC`.
- `src\oal\oallib\intr.c`: map `SYSINTR_AUDIO` <-> **IRQ 16 (`S5L_IRQ_DMAC0`, VIC0)** in
  `SysIntrToIrq`; in `OEMInterruptHandler` mask the line and return `SYSINTR_AUDIO`;
  `OEMInterruptDone` unmasks it (as CEonPi / as the UART0 entry does now).
- Hardware side (PL080 DMAC0 at 0x38200000, channel 2): set the terminal-count interrupt bit
  (bit 31) in each list item's `CH_CONTROL` and the ITC mask bit (bit 15) in `CH_CONFIG`
  (currently both interrupts masked: `CH_CONFIG_VALUE` in `ipodpcm.c`). Clear with
  `DMAC_INTTCCLR` (0x08) = `1 << 2`. Rockbox uses this interrupt (`INT_DMAC0` ->
  `dmac_callback`, `firmware/target/arm/s5l8702/dma-s5l8702.c`), but **this BSP has never used it:
  verify first**.
- Later caveat: the planned LCD DMA (DMAC0, peripheral 3) would share IRQ 16. Then the ISR must
  demultiplex by the controller's per-channel TC status, or one of them moves to DMAC1 (IRQ 17).

### 2. Hardware layer: DMA ring instead of two pages

- Reuse CEonPi's `pwmhw` design: **16 blocks x 256 frames** (~93 ms at 44.1 kHz), one list item
  per block, linked in a circle, interrupt per block (~5.8 ms). `Ack` clears finished blocks to
  silence (nothing stale ever repeats); read position (absolute frame) from `CH_SRCADDR`.
- Why: CEonPi's first design (whole-block fills, headers completed after playing) settled into
  alternating audio and silence with swmixer's few small buffers. See `Y:\CEonPi\docs\audio.md`
  "History".
- Sample format: 24 bits in 32-bit words (`I2STXCON_24IN32` = 0x0B100059), i.e. 16-bit sample
  `<< 8`; frame = left word, right word. Max 4095 words per list item (256 frames = 512 words, ok).
- Keep unchanged from `ipodpcm.c`: register mapping, the start-up set-up (I2S clock + TXCON +
  `I2SCLKDIV` 272, `CLKCON3` codec clock, codec reset on port 3 pin 3, CS42L55 register sequence
  through `I2C1:`, headphones at `IPODPCM_HP_DB` = -20 dB), DMAC0 clock gate / `DMAC_CONFIG`
  enable, channel off + idle wait, the "write PWRCON only when gated" rule (I2C1: toggles PWRCON1
  bit 4).
- DMA memory: `AllocPhysMem` uncached, as now (16 x 256 x 8 bytes = 32 KB + list items).

### 3. PDD (`wavepdd.cpp`), adapted from CEonPi

- Carry over: write cursor ahead of the read position (`WRITE_MARGIN`), headers completed when
  copied (advance `dwBytesRecorded`), `WPDM_ENDOFDATA` -> drain, `WPDM_CLOSE` waits for the drain
  (max 500 ms), underrun counting, app-buffer `__try`, `PDD_AudioGetInterruptType` logic, one
  stream at a time (`MMSYSERR_ALLOCATED`), output caps, software volume.
- Drop: PWM requantising and dither, PWM clock/range, Pi test tone (replace with an iPod one).
- Formats: **44100 Hz only**, PCM 8/16-bit, mono/stereo (swmixer always opens 44100/16/stereo).
  Hardware rate is 44118 Hz (12 MHz / 272), as today and as Rockbox. Per-open codec rate switching
  (Rockbox `sink_set_freq`, special case at 32 kHz) is possible later.
- `pfn_PDD_IoControl` -> `IOCTL_IPODPCM_GET_STATUS` (update `IPODPCM_STATUS` fields to the ring:
  interrupts, underruns, late, read/write position; bump `IPODPCM_VERSION`; update
  `src\apps\wavpanel\wavpanel.c`).

### 4. Build and clean-up

- `src\drivers\wavedev\sources`: `SOURCELIBS=$(_PUBLICROOT)\common\oak\lib\$(_CPUINDPATH)\wavemdd.lib`
  (public path, whole-archive, as CEonPi), `DLLENTRY=_DllEntryCRTStartup` (wavemdd defines
  `DllEntry`), sources = our hardware file + `wavepdd.cpp`. `ipodwav.def` exports `WAV_Init`,
  `WAV_Deinit`, `WAV_Open`, `WAV_Close`, `WAV_IOControl`, `WAV_PowerUp`, `WAV_PowerDown`.
- Delete `getms.ps1`, its call in `ipod6g.bat`, `src\drivers\wavedev\.gitignore` (list of
  Microsoft files), and the copied files / `getms.done` in the VM's tree.
- `files\platform.reg` (`HKLM\Drivers\BuiltIn\IpodWav`): keep Prefix/Dll/Index/`Order` 2/`IClass`;
  add `SysIntr` (= SYSINTR_AUDIO value), `Priority256` (CEonPi needed 110: above Ethernet 130,
  below USB 101; no Ethernet here, but keep it high), optional `TestTone`.
- Update `docs/drivers.md` (Sound section) and CLAUDE.md.
- CRLF + ASCII for every file (CLAUDE.md file rules).

## Decisions made

- Sample rates: 44100 only for now (mixer handles the rest).
- Volume: software gain first (as now and as CEonPi). Codec headphone level over `I2C1:` (1 dB
  steps, keeps resolution at low volume) as a separate later step.

## Steps, each testable on its own

1. **OAL interrupt + ring engine, with a registry `TestTone`** (play a tone at load from the
   ring, no MDD yet). Success: ~172 interrupts per second of tone (44118 / 256), correct pitch,
   none late; log the count and timing as CEonPi's `test tone done` line does.
2. **PDD on wavemdd.lib, `getms.ps1` removed.** Success: Windows start-up sound, Sound check
   tone and chime, two programs playing at once, no `underruns` in normal playback.
3. Optional: volume through the codec.

Test by launching `wince.ipod` from Rockbox (ROLO); also check one cold start, since the codec
set-up differs there (`I2SCLKDIV was 0`).
