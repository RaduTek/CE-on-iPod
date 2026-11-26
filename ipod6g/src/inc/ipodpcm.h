//
// ipodpcm.h - the status of the iPod's sound driver (ipodwav.dll), as its
// panel (wavpanel.exe, "Sound check") reads it, and the audio path's fixed
// settings. The driver's own interfaces are in src\drivers\wavedev\pcmring.h.
//
#ifndef IPODPCM_H
#define IPODPCM_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

// 3: the wave MDD + PDD driver with the DMA ring (2 was the two-page driver)
#define IPODPCM_VERSION             3

// The driver sets the codec up itself at start-up (the same after a Rockbox
// start and a cold start): 44.1 kHz, I2SCLKDIV = 12 MHz / 44100 = 272, so
// 44118 Hz (the codec divides its 12 MHz clock by 272 too). Windows opens
// the driver at 44100 Hz only: its software mixer converts everything else.
#define IPODPCM_RATE                44100

// The codec's fixed headphone level in dB (-60 .. +12; Rockbox's default
// volume is -25). Windows' volume scales the samples below this.
#define IPODPCM_HP_DB               (-20)

// The mixer makes 16-bit samples, the path takes 24-bit ones. A shift of 8
// is full scale (the first driver image used 6: 12 dB below it).
#define IPODPCM_HEADROOM_SHIFT      8

// DeviceIoControl on "WAV1:" with this code returns an IPODPCM_STATUS
#define IOCTL_IPODPCM_GET_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x920, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;
    DWORD   ready;          // registers mapped, ring memory there, codec set up
    DWORD   initRc;         // 0, or the step of the set-up that failed (4 = codec, 5 = ring)
    DWORD   clkDiv;         // I2SCLKDIV as the driver set it
    DWORD   rate;           // the sample rate that follows from it (12 MHz / clkDiv)
    DWORD   blocks;         // ring: blocks
    DWORD   blockFrames;    // ring: frames per block (one interrupt each)
    DWORD   running;        // the ring is playing
    DWORD   starts;
    DWORD   stops;
    // the stream (Windows' software mixer, normally 44100 Hz 16-bit stereo)
    DWORD   opens;
    DWORD   openRate;       // the last stream's format
    DWORD   openBits;
    DWORD   openChannels;
    DWORD   underruns;      // data came after the DMA had passed the write position
    DWORD   faults;         // an application buffer could not be read
    DWORD   volume;         // Windows' volume for the device (low word left, high right)
    // the ring and its interrupt
    DWORD   interrupts;     // ring interrupts handled, all runs
    DWORD   late;           // of those: 2 or more blocks finished (an interrupt served late)
    DWORD   zeroAcks;       // of those: no block finished
    DWORD   maxGapMs;       // longest time between two ring interrupts while playing
    DWORD   readPos;        // absolute frame the DMA reads (since the last start)
    DWORD   writePos;       // absolute frame the next data goes to
    DWORD   dmaErrors;      // times the DMA controller reported an error on our channel
    DWORD   stuckWaits;     // times the channel did not go idle when it was switched off
    DWORD   peak;           // largest sample since the last stop, in 16-bit units
    DWORD   lastPeak;       // the same, of the run before
    DWORD   spurious;       // interrupts while nothing played (DMA controller 0 quieted again)
    // registers, read when the status is asked for
    DWORD   position;       // DMA source address
    DWORD   bufPhys;        // the ring's physical address
    DWORD   pwrcon0;
    DWORD   pwrcon1;
    DWORD   clkcon3;
    DWORD   i2sTxCon;
    DWORD   i2sTxCom;
    DWORD   i2sStatus;
    DWORD   dmacConfig;
    DWORD   chConfig;
    DWORD   chControl;
    // the codec set-up
    DWORD   codecStep;      // 0 = done, else IPODPCM_CODEC_... where it stopped
    DWORD   codecReg;       // the codec register of that step
    DWORD   codecError;     // GetLastError of that step
    DWORD   codecVersion;   // the codec's register 0x01 (chip ID and revision)
    DWORD   codecWrites;    // registers written
    DWORD   hpReg;          // the headphone level written (HPACTL/HPBCTL)
} IPODPCM_STATUS;

// codecStep
#define IPODPCM_CODEC_OPEN          1   // I2C1: could not be opened
#define IPODPCM_CODEC_ADDRESS       2   // I2C1: refused the codec's address
#define IPODPCM_CODEC_ID            3   // the codec did not answer after its reset
#define IPODPCM_CODEC_WRITE         4   // a register write failed (codecReg)

#ifdef __cplusplus
}
#endif

#endif
