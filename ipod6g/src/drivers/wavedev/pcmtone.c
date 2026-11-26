//
// pcmtone.c - the DMA ring's test tone (pcmring.h), as CEonPi's pwmaudio
// bring-up tone
//
// With TestTone = 1 in the driver's registry key: 0.5 s of 440 Hz on the
// LEFT channel, then 0.5 s of 880 Hz on the RIGHT, written into the ring as
// it plays, one wait for SYSINTR_AUDIO per block. Runs in
// PDD_AudioInitialize, before the MDD takes the interrupt for its IST. The log line at the end
// says how many interrupts came in how long; at 44118 Hz one block of 256
// frames lasts 5.8 ms, so 1 s of tone is about 172 interrupts and none
// late.
//
// Integer sine: a 1024-entry table (filled with sin once) and a 32-bit
// phase per channel (no floating point per sample: the ARM926 has none).
//
#include <windows.h>
#include <nkintr.h>
#include <pkfuncs.h>
#include <math.h>

#include <ipodpcm.h>
#include "pcmring.h"

#define REG_TEST_TONE       L"TestTone"

#define TONE_TABLE_BITS     10
#define TONE_TABLE          (1 << TONE_TABLE_BITS)
#define TONE_AMPLITUDE      16384.0     // half of full scale
#define TONE_PI2            6.283185307179586
#define TONE_LEFT_HZ        440
#define TONE_RIGHT_HZ       880
#define TONE_WAIT_MS        500

static INT16 g_sine[TONE_TABLE];

static struct {
    DWORD   write;              // next absolute frame to write
    DWORD   leftEnd;            // 440 Hz LEFT before this frame
    DWORD   rightEnd;           // 880 Hz RIGHT before this frame, nothing after
    DWORD   phaseL, phaseR, incL, incR;
} g_tone;

static DWORD RegDword(HKEY hKey, LPCWSTR pszName, DWORD dwDefault)
{
    DWORD v, type, size = sizeof(v);

    if (hKey != NULL
        && RegQueryValueEx(hKey, pszName, NULL, &type, (LPBYTE)&v, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        return v;
    return dwDefault;
}

// phase step for hz at rate, 2^32 = one period
static DWORD PhaseStep(DWORD hz, DWORD rate)
{
    return (DWORD)(((unsigned __int64)hz << 32) / rate);
}

// The tone from the write cursor up to the ring's window end. The ring is
// silent wherever nothing is written (the other channel, after the tone).
static void ToneWrite(void)
{
    DWORD end = PcmRing_WindowEnd();
    INT32 *p;

    while ((int)(end - g_tone.write) > 0 && (int)(g_tone.rightEnd - g_tone.write) > 0)
    {
        p = PcmRing_FramePtr(g_tone.write);
        if ((int)(g_tone.leftEnd - g_tone.write) > 0)
        {
            p[0] = PCMRING_SAMPLE(g_sine[g_tone.phaseL >> (32 - TONE_TABLE_BITS)]);
            g_tone.phaseL += g_tone.incL;
        }
        else
        {
            p[1] = PCMRING_SAMPLE(g_sine[g_tone.phaseR >> (32 - TONE_TABLE_BITS)]);
            g_tone.phaseR += g_tone.incR;
        }
        g_tone.write++;
    }
}

static void ToneRun(DWORD sysIntr, DWORD priority)
{
    HANDLE hEvent;
    PCMRING_COUNTERS c;
    DWORD rate, start, last, now, gap, maxGap = 0, waits = 0, timeouts = 0, elapsed, i;
    int oldPriority, n = 0;

    rate = IpodPcm_Rate();
    for (i = 0; i < TONE_TABLE; i++)
        g_sine[i] = (INT16)(sin(TONE_PI2 * i / TONE_TABLE) * TONE_AMPLITUDE);

    hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hEvent == NULL)
        return;
    if (!InterruptInitialize(sysIntr, hEvent, NULL, 0))
    {
        RETAILMSG(1, (L"ipodwav: test tone: InterruptInitialize(%u) failed, error %u\r\n",
                      sysIntr, GetLastError()));
        CloseHandle(hEvent);
        return;
    }

    // refill at the sound IST's priority
    oldPriority = CeGetThreadPriority(GetCurrentThread());
    CeSetThreadPriority(GetCurrentThread(), priority);

    memset(&g_tone, 0, sizeof(g_tone));
    g_tone.leftEnd = rate / 2;
    g_tone.rightEnd = rate;
    g_tone.incL = PhaseStep(TONE_LEFT_HZ, rate);
    g_tone.incR = PhaseStep(TONE_RIGHT_HZ, rate);
    PcmRing_Reset();
    ToneWrite();

    RETAILMSG(1, (L"ipodwav: test tone: 0.5 s %u Hz LEFT, then 0.5 s %u Hz RIGHT at %u Hz, SysIntr %u, priority %u\r\n",
                  TONE_LEFT_HZ, TONE_RIGHT_HZ, rate, sysIntr, priority));
    if (!PcmRing_Start())
    {
        RETAILMSG(1, (L"ipodwav: test tone: the ring did not start\r\n"));
        goto Done;
    }
    // no register dump here: ~400 characters of RETAILMSG over the serial
    // port hold this thread for ~35 ms (step 1 measured that as a gap)
    start = last = GetTickCount();

    // keep writing until all of the tone has been played
    while ((int)(PcmRing_ReadPos() - g_tone.rightEnd) < 0)
    {
        if (WaitForSingleObject(hEvent, TONE_WAIT_MS) != WAIT_OBJECT_0)
        {
            RETAILMSG(1, (L"ipodwav: test tone: no DMA interrupt within %u ms\r\n", TONE_WAIT_MS));
            PcmRing_Dump(L"timeout");
            if (++timeouts >= 2)
                break;
            continue;
        }
        now = GetTickCount();
        gap = now - last;
        last = now;
        if (gap > maxGap)
            maxGap = gap;
        waits++;

        n = PcmRing_Ack();
        InterruptDone(sysIntr);
        if (n < 0)
            break;
        ToneWrite();
    }
    elapsed = GetTickCount() - start;
    PcmRing_Counters(&c);
    PcmRing_Dump(L"end");
    PcmRing_Stop();

    // one interrupt per block of PCMRING_BLOCK_FRAMES frames; 1 s of tone
    RETAILMSG(1, (L"ipodwav: test tone done: %u DMA interrupts in %u ms (expect ~%u in ~%u ms), longest gap %u ms, %u late, %u empty, %u timeouts, %u DMA errors, %u other channels\r\n",
                  waits, elapsed, g_tone.rightEnd / PCMRING_BLOCK_FRAMES,
                  g_tone.rightEnd * 1000 / rate, maxGap, c.late, c.zeroAcks, timeouts,
                  c.dmaErrors, c.otherChannels));

Done:
    CeSetThreadPriority(GetCurrentThread(), oldPriority);
    InterruptDisable(sysIntr);
    CloseHandle(hEvent);
}

void PcmTone_Check(HKEY hKey, DWORD sysIntr, DWORD priority)
{
    if (RegDword(hKey, REG_TEST_TONE, 0))
        ToneRun(sysIntr, priority);
}
