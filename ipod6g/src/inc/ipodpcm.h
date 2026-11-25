//
// ipodpcm.h - the iPod's audio path, as the hardware layer of the Windows CE
// sound driver (ipodwav.dll) and as the status its panel reads
//
#ifndef IPODPCM_H
#define IPODPCM_H

#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IPODPCM_VERSION             2

// One page is 20 ms of sound, and one DMA list item: at most 4095 words, so
// at most 2047 stereo frames. 1920 frames is 20 ms at 96 kHz.
#define IPODPCM_MAX_PAGE_FRAMES     1920
#define IPODPCM_FALLBACK_RATE       48000
#define IPODPCM_FALLBACK_FRAMES     960

// The driver sets the codec up itself at start-up (the same after a Rockbox
// start and a cold start): 44.1 kHz, I2SCLKDIV = 12 MHz / 44100 = 272, so
// 44118 Hz (the codec divides its 12 MHz clock by 272 too).
#define IPODPCM_RATE                44100

// The codec's fixed headphone level in dB (-60 .. +12; Rockbox's default
// volume is -25). Windows' volume scales the samples below this.
#define IPODPCM_HP_DB               (-20)

// The mixer makes 16-bit samples, the path takes 24-bit ones. A shift of 8
// is full scale (the first driver image used 6: 12 dB below it).
#define IPODPCM_HEADROOM_SHIFT      8

// how often the driver's thread looks at the DMA position while playing
#define IPODPCM_POLL_MS             2

// DeviceIoControl on "WAV1:" with this code returns an IPODPCM_STATUS
#define IOCTL_IPODPCM_GET_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x920, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct {
    DWORD   version;
    DWORD   ready;          // registers mapped, memory there, codec set up
    DWORD   initRc;         // 0, or the step of IpodPcm_Init that failed (4 = codec)
    DWORD   clkDiv;         // I2SCLKDIV as the driver set it
    DWORD   rate;           // the sample rate that follows from it (12 MHz / clkDiv)
    DWORD   pageFrames;     // frames per page
    DWORD   running;
    DWORD   starts;
    DWORD   stops;
    DWORD   lastStartRc;    // 0, or why the last start was refused
    DWORD   fills;          // pages handed over
    DWORD   lateFills;      // pages handed over while they were already being played
    DWORD   maxGapMs;       // longest time between two looks at the DMA position while playing
    DWORD   stalls;         // times the position stood still for half a second
    DWORD   dmaErrors;      // times the DMA controller reported an error on our channel
    DWORD   stuckWaits;     // times the channel did not go idle when it was switched off
    DWORD   peak;           // largest sample since the last stop, in 16-bit units
    DWORD   lastPeak;       // the same, of the run before
    DWORD   position;       // DMA source address now
    DWORD   bufPhys;
    DWORD   pwrcon0;
    DWORD   pwrcon1;
    DWORD   clkcon3;
    DWORD   i2sTxCon;
    DWORD   i2sTxCom;
    DWORD   i2sStatus;
    DWORD   dmacConfig;
    DWORD   chConfig;
    DWORD   chControl;
    // the codec set-up (version 2)
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

// Maps the registers, gets the DMA memory, sets up I2S and the codec
// (reset, Rockbox's register sequence, unmuted at IPODPCM_HP_DB) through
// "I2C1:". Starts no DMA. FALSE = playback is not possible (see
// initRc); the other functions are safe to call all the same.
BOOL  IpodPcm_Init(void);

// Never 0: the fallback values when the path is not usable.
DWORD IpodPcm_Rate(void);
DWORD IpodPcm_PageFrames(void);

// Hands one page (0 or 1) of IpodPcm_PageFrames() stereo 16-bit frames over.
void  IpodPcm_WritePage(DWORD page, const short *pStereo);

// Both pages must have been handed over. Plays page 0, 1, 0, 1 ... until
// IpodPcm_Stop.
BOOL  IpodPcm_Start(void);
void  IpodPcm_Stop(void);

// TRUE when the DMA controller is not reading from this page (so it may be
// refilled). To be called every IPODPCM_POLL_MS while playing.
BOOL  IpodPcm_PageFinished(DWORD page);

BOOL  IpodPcm_StatusIoctl(PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut);

#ifdef __cplusplus
}
#endif

#endif
