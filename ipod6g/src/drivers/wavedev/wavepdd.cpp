//
// wavepdd.cpp - the PDD of the iPod's sound driver (ipodwav.dll)
//
// Microsoft's wave MDD (wavemdd.lib, linked from the public tree) is the
// upper half: the WAV_* stream entry points, the wave API messages, the
// header queue and the interrupt thread (IST) on gIntrAudio. This file is
// the lower half, over the DMA ring of the hardware layer (pcmring.h,
// ipodpcm.c). The model is CEonPi's pwmaudio driver (Y:\CEonPi\raspi2\src\
// drivers\pwmaudio\wavepdd.cpp, docs\audio.md there).
//
// Playback: one output stream at a time (the MDD keeps a single stream
// state); no input device. Formats: PCM, 8 or 16 bit, mono or stereo,
// 44100 Hz only (the hardware runs at 44118 Hz, 12 MHz / 272). CE 7's
// software mixer in waveapi.dll sits in front of the driver: it mixes all
// programs' sounds, converts their rates and opens the driver at 44100 Hz
// 16-bit stereo, recycling a few small buffers (the next one comes when
// the driver returns one).
//
// Write cursor: the DMA loops over a ~93 ms ring that is cleared to silence
// behind it. Audio from the queued WAVEHDRs is copied into the ring at an
// absolute frame position (g_out.write) ahead of the DMA read position, and
// each header's dwBytesRecorded is advanced as its bytes are copied. The
// MDD treats a header whose dwBytesRecorded reached dwBufferLength as done
// and returns it on the next interrupt (every ~5.8 ms), so the mixer gets
// its buffers back quickly and the ring is never padded with silence while
// data exists. Silence plays only when no data is queued (counted as an
// underrun when data then comes late).
//
// Consequences: waveOutGetPosition counts bytes copied, ahead of the sound
// by up to the ring (~93 ms); waveOutPause drops what was copied but not
// played. At the end of the data the MDD sends WPDM_ENDOFDATA as soon as
// the last header is copied: the DMA runs on until the copied audio has
// played ("draining"), and WPDM_CLOSE waits for that, so the end of a sound
// is not cut off.
//
// Volume: Windows' device volume scales the samples (0xFFFF = unity) below
// the codec's fixed headphone level (IPODPCM_HP_DB).
//
// IOCTL_IPODPCM_GET_STATUS (ipodpcm.h) on WAV1: reaches the PDD through the
// MDD's pfn_PDD_IoControl hook; the Sound check panel reads it.
//
// Registry (the driver's key): SysIntr (SYSINTR_AUDIO), Priority256 (also
// the MDD's IST priority; the MDD's own default is 210), TestTone (1 = the
// 440/880 Hz tone at start-up, pcmtone.c).
//
#include <windows.h>
#include <types.h>
#include <nkintr.h>
#include <ceddk.h>
#include <devload.h>
#include <wavedbg.h>
#include <waveddsi.h>

#include <bsp_cfg.h>
#include <ipodpcm.h>
#include "pcmring.h"

#define REG_SYSINTR         L"SysIntr"
#define REG_PRIORITY        L"Priority256"   // also read by the MDD for its IST
#define DEFAULT_PRIORITY    110             // as CEonPi's sound IST

#define CLOSE_DRAIN_MS      500             // longest WPDM_CLOSE waits for the ring to play out

// The MDD's interrupt thread waits on this SYSINTR (waveddsi.h).
DWORD gIntrAudio = SYSINTR_AUDIO;

static CRITICAL_SECTION g_cs;               // protects the ring and g_out
static BOOL             g_fInit;            // g_cs exists
static BOOL             g_fHw;              // the hardware layer is set up
static ULONG            g_ulVolume = 0xFFFFFFFF;   // low word left, high word right
static IPODPCM_STATUS  *g_pSt;

// Output stream state
static struct {
    BOOL  fOpen;                            // a stream is open (one at a time)
    BOOL  fPlaying;                         // DMA running for it
    BOOL  fDraining;                        // all data copied, playing it out
    DWORD write;                            // absolute ring frame for the next data
    WORD  wChannels;
    WORD  wBits;
    WORD  wBlockAlign;                      // bytes per source frame
    DWORD underruns;                        // this stream's (the status keeps the total)
    DWORD faults;
    DWORD lastInt;                          // GetTickCount of the last ring interrupt
} g_out;

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
static DWORD RegReadDword(HKEY hKey, LPCWSTR pszName, DWORD dwDefault)
{
    DWORD dwValue, dwType, dwSize = sizeof(dwValue);

    if (hKey != NULL
        && RegQueryValueEx(hKey, pszName, NULL, &dwType, (LPBYTE)&dwValue, &dwSize) == ERROR_SUCCESS
        && dwType == REG_DWORD)
        return dwValue;
    return dwDefault;
}

// ---------------------------------------------------------------------------
// Sample conversion and the write cursor (called with g_cs held)
// ---------------------------------------------------------------------------

// 0..0xFFFF volume word to a 0..0x10000 gain (0xFFFF is unity).
static __inline int VolumeGain(WORD w)
{
    return (int)w + (w >> 15);
}

// Converts nFrames source frames at pSrc to ring words (left, right) at pOut.
static void ConvertFrames(const BYTE *pSrc, INT32 *pOut, UINT nFrames)
{
    int gainL = VolumeGain(LOWORD(g_ulVolume));
    int gainR = VolumeGain(HIWORD(g_ulVolume));
    DWORD peak = g_pSt->peak;
    UINT i;

    for (i = 0; i < nFrames; i++)
    {
        int l, r, a;

        if (g_out.wBits == 16)
        {
            // byte reads: the application's buffer need not be 2-byte aligned
            l = (short)(pSrc[0] | (pSrc[1] << 8));
            r = (g_out.wChannels == 2) ? (short)(pSrc[2] | (pSrc[3] << 8)) : l;
        }
        else
        {
            l = ((int)pSrc[0] - 128) << 8;
            r = (g_out.wChannels == 2) ? (((int)pSrc[1] - 128) << 8) : l;
        }
        pSrc += g_out.wBlockAlign;

        l = (l * gainL) >> 16;
        r = (r * gainR) >> 16;
        a = (l < 0) ? -l : l;
        if ((DWORD)a > peak)
            peak = (DWORD)a;
        a = (r < 0) ? -r : r;
        if ((DWORD)a > peak)
            peak = (DWORD)a;

        pOut[2 * i] = PCMRING_SAMPLE(l);
        pOut[2 * i + 1] = PCMRING_SAMPLE(r);
    }
    g_pSt->peak = peak;
}

// TRUE if a header in the chain still has bytes to copy.
static BOOL HaveData(PWAVEHDR pwh)
{
    for (; pwh != NULL; pwh = pwh->lpNext)
    {
        if (pwh->dwBytesRecorded < pwh->dwBufferLength)
            return TRUE;
    }
    return FALSE;
}

// Copies queued audio into the ring at the write cursor, as far as the ring
// window allows, advancing each header's dwBytesRecorded. A partial frame at
// the end of a header (length not a multiple of nBlockAlign) is skipped but
// counted, so the header still completes.
static void WriteFromHeaders(PWAVEHDR pwh)
{
    DWORD end = PcmRing_WindowEnd();

    if (!HaveData(pwh))
        return;

    // Never write behind the DMA: if the cursor fell behind (no data for a
    // while), restart it just ahead of the read position.
    if (PcmRing_Running())
    {
        DWORD min = PcmRing_ReadPos() + PCMRING_WRITE_MARGIN;
        if ((int)(g_out.write - min) < 0)
        {
            g_out.underruns++;
            g_pSt->underruns++;
            g_out.write = min;
        }
    }

    while (pwh != NULL && (int)(end - g_out.write) > 0)
    {
        DWORD avail = (pwh->dwBytesRecorded < pwh->dwBufferLength) ?
                      pwh->dwBufferLength - pwh->dwBytesRecorded : 0;
        DWORD frames, room;

        if (avail < g_out.wBlockAlign)
        {
            pwh->dwBytesRecorded = pwh->dwBufferLength;
            pwh = pwh->lpNext;
            continue;
        }

        frames = avail / g_out.wBlockAlign;
        room = end - g_out.write;
        if (frames > room)
            frames = room;
        room = PcmRing_Contiguous(g_out.write);
        if (frames > room)
            frames = room;

        __try
        {
            ConvertFrames((const BYTE *)pwh->lpData + pwh->dwBytesRecorded,
                          PcmRing_FramePtr(g_out.write), frames);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // the application's buffer went away: play silence for it
            memset(PcmRing_FramePtr(g_out.write), 0, frames * 2 * sizeof(INT32));
            g_out.faults++;
            g_pSt->faults++;
        }

        pwh->dwBytesRecorded += frames * g_out.wBlockAlign;
        g_out.write += frames;
    }
    g_pSt->writePos = g_out.write;
}

// TRUE once the DMA has played everything written.
static BOOL DrainDone(void)
{
    return (int)(PcmRing_ReadPos() - g_out.write) >= 0;
}

// Stops the ring for the stream (g_cs held).
static void StopLocked(void)
{
    if (g_out.fPlaying)
    {
        PcmRing_Stop();
        g_out.fPlaying = FALSE;
        g_out.fDraining = FALSE;
    }
}

// ---------------------------------------------------------------------------
// Output control (called with the MDD's output lock held, takes g_cs)
// ---------------------------------------------------------------------------
static void OutStart(PWAVEHDR pwh)
{
    EnterCriticalSection(&g_cs);

    g_out.fDraining = FALSE;
    if (g_out.fPlaying)
    {
        // new data while the previous data is still playing out: append
        WriteFromHeaders(pwh);
    }
    else
    {
        PcmRing_Reset();
        g_out.write = 0;
        WriteFromHeaders(pwh);
        if (PcmRing_Start())
        {
            g_out.fPlaying = TRUE;
            g_out.lastInt = GetTickCount();
        }
    }

    LeaveCriticalSection(&g_cs);
}

static void OutContinue(PWAVEHDR pwh)
{
    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying)
        WriteFromHeaders(pwh);
    LeaveCriticalSection(&g_cs);
}

static void OutStop(void)
{
    EnterCriticalSection(&g_cs);
    StopLocked();
    LeaveCriticalSection(&g_cs);
}

static void OutEndOfData(void)
{
    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying)
        g_out.fDraining = TRUE;             // PDD_AudioGetInterruptType stops the ring
    LeaveCriticalSection(&g_cs);
}

static MMRESULT OutOpen(LPWAVEFORMATEX pwfx, BOOL fQueryOnly)
{
    MMRESULT mmRet = MMSYSERR_NOERROR;

    if (!g_fHw)
        return MMSYSERR_NOTENABLED;
    if (pwfx == NULL)
        return WAVERR_BADFORMAT;

    if (pwfx->wFormatTag != WAVE_FORMAT_PCM
        || (pwfx->nChannels != 1 && pwfx->nChannels != 2)
        || (pwfx->wBitsPerSample != 8 && pwfx->wBitsPerSample != 16)
        || pwfx->nSamplesPerSec != IPODPCM_RATE
        || pwfx->nBlockAlign != pwfx->nChannels * pwfx->wBitsPerSample / 8)
        return WAVERR_BADFORMAT;

    if (fQueryOnly)
        return MMSYSERR_NOERROR;

    EnterCriticalSection(&g_cs);
    if (g_out.fOpen)
    {
        // the MDD keeps one stream state and does not check this itself
        mmRet = MMSYSERR_ALLOCATED;
    }
    else
    {
        g_out.fOpen = TRUE;
        g_out.wChannels = pwfx->nChannels;
        g_out.wBits = pwfx->wBitsPerSample;
        g_out.wBlockAlign = pwfx->nBlockAlign;
        g_out.underruns = 0;
        g_out.faults = 0;
        g_pSt->opens++;
        g_pSt->openRate = pwfx->nSamplesPerSec;
        g_pSt->openBits = pwfx->wBitsPerSample;
        g_pSt->openChannels = pwfx->nChannels;
    }
    LeaveCriticalSection(&g_cs);

    if (mmRet == MMSYSERR_NOERROR)
        RETAILMSG(1, (L"ipodwav: open %u Hz %u-bit %s\r\n", pwfx->nSamplesPerSec,
                      pwfx->wBitsPerSample, (pwfx->nChannels == 2) ? L"stereo" : L"mono"));
    return mmRet;
}

static void OutClose(void)
{
    DWORD start = GetTickCount();

    // Let the copied audio play out (at most one ring, ~93 ms), so the end
    // of a sound is not cut off. The IST keeps acknowledging the ring
    // meanwhile: the MDD calls PDD_AudioGetInterruptType without its lock.
    for (;;)
    {
        BOOL fWait;

        EnterCriticalSection(&g_cs);
        fWait = g_out.fPlaying && !DrainDone();
        LeaveCriticalSection(&g_cs);
        if (!fWait || GetTickCount() - start > CLOSE_DRAIN_MS)
            break;
        Sleep(5);
    }

    EnterCriticalSection(&g_cs);
    StopLocked();
    if (g_out.underruns || g_out.faults)
        RETAILMSG(1, (L"ipodwav: close, %u underruns, %u application buffer faults\r\n",
                      g_out.underruns, g_out.faults));
    g_out.fOpen = FALSE;
    LeaveCriticalSection(&g_cs);
}

static MMRESULT GetOutCaps(PWAVEOUTCAPS pCaps)
{
    // GETDEVCAPS(NULL) is the MDD's "is there an output device": without the
    // hardware there is none, but the driver stays loaded for the panel
    if (!g_fHw)
        return MMSYSERR_NOTENABLED;
    if (pCaps == NULL)
        return MMSYSERR_NOERROR;

    memset(pCaps, 0, sizeof(*pCaps));
    pCaps->wMid = MM_MICROSOFT;
    pCaps->wPid = 24;                       // generic wave out
    pCaps->vDriverVersion = 0x0001;
    wcsncpy(pCaps->szPname, L"iPod headphones", MAXPNAMELEN - 1);
    pCaps->dwFormats = WAVE_FORMAT_4M08 | WAVE_FORMAT_4M16 |
                       WAVE_FORMAT_4S08 | WAVE_FORMAT_4S16;
    pCaps->wChannels = 2;
    pCaps->dwSupport = WAVECAPS_VOLUME | WAVECAPS_LRVOLUME;
    return MMSYSERR_NOERROR;
}

// ---------------------------------------------------------------------------
// The status IOCTL (through the MDD's pfn_PDD_IoControl)
// ---------------------------------------------------------------------------
BOOL PDD_IoControl(PDWORD pdwOpenData, DWORD dwCode, PBYTE pBufIn, DWORD dwLenIn,
                   PBYTE pBufOut, DWORD dwLenOut, PDWORD pdwActualOut)
{
    BOOL rc;

    UNREFERENCED_PARAMETER(pdwOpenData);
    UNREFERENCED_PARAMETER(pBufIn);
    UNREFERENCED_PARAMETER(dwLenIn);

    if (dwCode != IOCTL_IPODPCM_GET_STATUS)
    {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    EnterCriticalSection(&g_cs);
    g_pSt->volume = g_ulVolume;
    rc = IpodPcm_StatusIoctl(pBufOut, dwLenOut, pdwActualOut);
    LeaveCriticalSection(&g_cs);
    return rc;
}

// ---------------------------------------------------------------------------
// PDD entry points (waveddsi.h)
// ---------------------------------------------------------------------------
BOOL PDD_AudioInitialize(DWORD dwIndex)
{
    HKEY hKey;
    DWORD priority;

    // the MDD passes the Device Manager active key path as dwIndex
    hKey = OpenDeviceKey((LPCWSTR)dwIndex);
    gIntrAudio = RegReadDword(hKey, REG_SYSINTR, SYSINTR_AUDIO);
    priority = RegReadDword(hKey, REG_PRIORITY, DEFAULT_PRIORITY);

    InitializeCriticalSection(&g_cs);
    g_fInit = TRUE;
    memset(&g_out, 0, sizeof(g_out));
    pfn_PDD_IoControl = PDD_IoControl;

    // A failed set-up is not fatal: the driver loads with no output device
    // (GetOutCaps), and the panel's status says why.
    g_fHw = IpodPcm_Init();
    g_pSt = IpodPcm_Status();
    RETAILMSG(1, (L"ipodwav: wave MDD + PDD, SysIntr %u, Priority256 %u, %s\r\n",
                  gIntrAudio, priority, g_fHw ? L"ready" : L"NO OUTPUT DEVICE (set-up failed)"));

    // before the MDD's InterruptInitialize on gIntrAudio
    if (g_fHw)
        PcmTone_Check(hKey, gIntrAudio, priority);

    if (hKey != NULL)
        RegCloseKey(hKey);
    return TRUE;
}

VOID PDD_AudioDeinitialize(VOID)
{
    if (!g_fInit)
        return;
    OutStop();
    pfn_PDD_IoControl = NULL;
    // the ring's memory and the mappings stay: the driver is never unloaded
    DeleteCriticalSection(&g_cs);
    g_fInit = FALSE;
    g_fHw = FALSE;
}

VOID PDD_AudioPowerHandler(BOOL power_down)
{
    // Called by the MDD's power handler (no blocking calls allowed, so no
    // lock). On power-down only the DMA is stopped; the MDD marks the queued
    // headers done and restarts the stream on the next write after power-up.
    if (power_down && g_out.fPlaying)
    {
        PcmRing_Stop();
        g_out.fPlaying = FALSE;
        g_out.fDraining = FALSE;
    }
}

AUDIO_STATE PDD_AudioGetInterruptType(VOID)
{
    AUDIO_STATE state = AUDIO_STATE_IGNORE;
    DWORD now, gap;
    int done;

    if (!g_fInit)
        return state;

    EnterCriticalSection(&g_cs);
    if (g_out.fPlaying)
    {
        now = GetTickCount();
        gap = now - g_out.lastInt;
        g_out.lastInt = now;
        if (gap > g_pSt->maxGapMs)
            g_pSt->maxGapMs = gap;

        done = PcmRing_Ack();
        if (done < 0)
        {
            // DMA error (logged, ring stopped by PcmRing_Ack): end the stream
            g_out.fPlaying = FALSE;
            g_out.fDraining = FALSE;
            state = AUDIO_STATE_OUT_STOPPED;
        }
        else if (g_out.fDraining)
        {
            // The MDD has no more data. Stop once the ring has played out;
            // until then there is nothing for the MDD to do.
            if (DrainDone())
                StopLocked();
        }
        else if (done > 0)
        {
            // return the copied headers and copy more (WPDM_CONTINUE)
            state = AUDIO_STATE_OUT_PLAYING;
        }
    }
    else if (g_fHw)
    {
        // Nothing plays, so nothing of ours should interrupt. Whatever holds
        // the shared DMA controller 0 line up must be cleared now, or the
        // MDD's InterruptDone lets it straight back in (an endless storm
        // at this thread's priority stopped the boot once).
        PcmRing_Quiet();
        g_pSt->spurious++;
    }
    LeaveCriticalSection(&g_cs);
    return state;
}

DWORD PDD_AudioMessage(UINT uMsg, DWORD dwParam1, DWORD dwParam2)
{
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(dwParam1);
    UNREFERENCED_PARAMETER(dwParam2);
    return MMSYSERR_NOTSUPPORTED;
}

MMRESULT PDD_WaveProc(WAPI_INOUT apidir, DWORD dwCode, DWORD dwParam1, DWORD dwParam2)
{
    // no input: GETDEVCAPS(NULL) failing makes the MDD report 0 input devices
    if (apidir != WAPI_OUT)
        return MMSYSERR_NOTSUPPORTED;

    switch (dwCode)
    {
    case WPDM_GETDEVCAPS:
        return GetOutCaps((PWAVEOUTCAPS)dwParam1);

    case WPDM_OPEN:
        return OutOpen((LPWAVEFORMATEX)dwParam1, (BOOL)dwParam2);

    case WPDM_CLOSE:
        OutClose();
        return MMSYSERR_NOERROR;

    case WPDM_START:
    case WPDM_RESTART:
        if (dwParam1)
            OutStart((PWAVEHDR)dwParam1);
        return MMSYSERR_NOERROR;

    case WPDM_CONTINUE:
        if (dwParam1)
            OutContinue((PWAVEHDR)dwParam1);
        return MMSYSERR_NOERROR;

    case WPDM_ENDOFDATA:                    // every queued byte has been copied
        OutEndOfData();
        return MMSYSERR_NOERROR;

    case WPDM_PAUSE:                        // copied but unplayed audio is dropped
    case WPDM_STOP:                         // waveOutReset
        OutStop();
        return MMSYSERR_NOERROR;

    case WPDM_STANDBY:
        return MMSYSERR_NOERROR;

    case WPDM_GETVOLUME:
        if (dwParam1)
            *(PULONG)dwParam1 = g_ulVolume;
        return MMSYSERR_NOERROR;

    case WPDM_SETVOLUME:
        g_ulVolume = dwParam1;
        return MMSYSERR_NOERROR;

    default:
        return MMSYSERR_NOTSUPPORTED;
    }
}
