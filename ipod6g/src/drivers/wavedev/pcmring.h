//
// pcmring.h - the DMA ring of the iPod's sound driver (ipodpcm.c) and its
// test tone (pcmtone.c). Internal to ipodwav.dll.
//
// DMA controller 0, channel 2 loops over a ring of PCMRING_BLOCKS blocks of
// PCMRING_BLOCK_FRAMES frames, one list item per block, the last linked
// back to the first. Each list item raises the channel's terminal count
// interrupt when it is done (SYSINTR_AUDIO, about every 5.8 ms at 44.1 kHz).
//
// The ring is a write cursor buffer (as CEonPi's pwmaudio): when a block has
// been played, PcmRing_Ack clears it to silence, so a part nobody wrote
// plays as silence and nothing stale is ever repeated. The owner writes
// frames at absolute frame positions between PcmRing_ReadPos() (+ a margin)
// and PcmRing_WindowEnd(). Positions count frames since PcmRing_Start
// (DWORD, compare them as differences: they wrap).
//
// A frame is two 32-bit words, left then right, each a 24-bit sample in the
// low bits of the word (I2S 0 set to 24 bits in 32): PCMRING_SAMPLE.
//
// All functions are called with the owner's lock held (or from the only
// thread using the ring).
//
#ifndef PCMRING_H
#define PCMRING_H

#ifdef __cplusplus
extern "C" {
#endif

#define PCMRING_BLOCKS          16
#define PCMRING_BLOCK_FRAMES    256     // 512 words: one list item (max 4095 words)
#define PCMRING_FRAMES          (PCMRING_BLOCKS * PCMRING_BLOCK_FRAMES)    // ~93 ms

// Frames kept between the DMA read position and the first frame written:
// the DMA controller reads ahead (bursts, the I2S FIFO).
#define PCMRING_WRITE_MARGIN    64

// a signed 16-bit sample as a ring word
#define PCMRING_SAMPLE(s)       ((INT32)(s) * (1 << IPODPCM_HEADROOM_SHIFT))

typedef struct {
    DWORD   interrupts;     // PcmRing_Ack calls while running
    DWORD   late;           // acks that found 2 or more blocks finished
    DWORD   zeroAcks;       // acks that found no block finished
    DWORD   dmaErrors;      // DMA errors on our channel (the ring stops)
    DWORD   otherChannels;  // acks that saw another channel's interrupt on DMAC0
} PCMRING_COUNTERS;

// After IpodPcm_Init succeeded: the ring's memory and list items. FALSE =
// no ring (IpodPcm_Init failed, or no memory).
BOOL   PcmRing_Init(void);

// While stopped: the whole ring silent, positions 0, so frames
// 0 .. PCMRING_FRAMES-1 can be written before PcmRing_Start.
void   PcmRing_Reset(void);

// Clocks, I2S transmitter, then the channel from block 0 (frame 0), with
// its interrupts on. FALSE = not set up, or the two-page engine is playing.
BOOL   PcmRing_Start(void);
void   PcmRing_Stop(void);
BOOL   PcmRing_Running(void);

// From the IST on SYSINTR_AUDIO, before InterruptDone: clears the channel's
// terminal count, clears every finished block to silence and moves the
// play position on. Returns the number of finished blocks (normally 1), or
// -1 when not running or on a DMA error (the ring is then stopped).
int    PcmRing_Ack(void);

// Absolute frame the DMA controller reads now (the stop position when stopped).
DWORD  PcmRing_ReadPos(void);

// First absolute frame that may NOT be written (one ring after the start of
// the playing block).
DWORD  PcmRing_WindowEnd(void);

// The two words of absolute frame dwFrame; PcmRing_Contiguous(dwFrame)
// frames follow it without wrapping.
INT32 *PcmRing_FramePtr(DWORD dwFrame);
DWORD  PcmRing_Contiguous(DWORD dwFrame);

void   PcmRing_Counters(PCMRING_COUNTERS *pc);

// Logs the DMA controller, channel and VIC registers (bring-up).
void   PcmRing_Dump(LPCWSTR pszWhen);

// pcmtone.c: with registry value TestTone = 1 under hKey (or, hKey NULL,
// HKLM\Drivers\BuiltIn\IpodWav), plays 0.5 s of 440 Hz LEFT, then 0.5 s of
// 880 Hz RIGHT through the ring and SYSINTR_AUDIO, and logs how the
// interrupts came. Also reads SysIntr and Priority256 there.
void   PcmTone_Check(HKEY hKey);

#ifdef __cplusplus
}
#endif

#endif
