#
# getms.ps1 - the Microsoft part of the iPod's sound driver, from your own
# Platform Builder 7
#
# ipodwav.dll is Microsoft's sample driver wavedev2\virtual (mixer, volume,
# sample rate conversion; its hardware layer is empty) with the iPod's audio
# path (ipodpcm.c) put into that hardware layer. Microsoft's files are not in
# this repository: this script copies them from
#     %_PUBLICROOT%\common\oak\drivers\wavedev\wavedev2\virtual
# into this folder and makes the small changes below to three of them.
# ipod6g.bat runs it when getms.done is missing; delete getms.done to run it
# again. Windows PowerShell 2.0. Every change must match exactly once, or
# nothing is written and the script fails.
#
$ErrorActionPreference = 'Stop'
$dest = Split-Path -Parent $MyInvocation.MyCommand.Path
$src = Join-Path $env:_PUBLICROOT 'common\oak\drivers\wavedev\wavedev2\virtual'
$latin1 = [Text.Encoding]::GetEncoding(28591)

function Fail([string]$why)
{
    Write-Host "getms.ps1: $why"
    [Environment]::Exit(1)
}

# a file of the sample, lines without trailing blanks, LF
function Read-Sample([string]$name)
{
    $t = [IO.File]::ReadAllText((Join-Path $src $name), $latin1).Replace("`r`n", "`n")
    $lines = $t.Split("`n")
    for ($i = 0; $i -lt $lines.Length; $i++) { $lines[$i] = $lines[$i].TrimEnd() }
    return [string]::Join("`n", $lines)
}

function Edit([string]$name, [string]$text, [string]$old, [string]$new)
{
    $old = $old.Replace("`r`n", "`n")
    $new = $new.Replace("`r`n", "`n")
    $at = $text.IndexOf($old)
    if ($at -lt 0 -or $text.IndexOf($old, $at + 1) -ge 0) { Fail "$($name): a change does not fit this Platform Builder's sample" }
    return $text.Substring(0, $at) + $new + $text.Substring($at + $old.Length)
}

if (-not $env:_PUBLICROOT -or -not (Test-Path $src)) { Fail "Microsoft's sample is not at $src" }

$asIs = @('devctxt.cpp', 'devctxt.h', 'dtpnotify.cpp', 'dtpnotify.h', 'input.cpp', 'mixerdrv.cpp', 'mixerdrv.h', 'oemsettings.h', 'output.cpp', 'streamclass.cpp', 'streamclass.h', 'strmctxt.cpp', 'strmctxt.h', 'wavemain.h', 'wavepdd.h', 'wavertgctrl.cpp')
$edited = @{}
$edited['hwctxt.cpp'] = Read-Sample 'hwctxt.cpp'
$edited['hwctxt.h'] = Read-Sample 'hwctxt.h'
$edited['wavemain.cpp'] = Read-Sample 'wavemain.cpp'

# hwctxt.cpp, change 1
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
BOOL HardwareContext::MapRegisters()
{
    return(TRUE);
}
'@ @'
BOOL HardwareContext::MapRegisters()
{
    // iPod: map the audio path and read the sample rate Rockbox left the codec
    // at. A failure is not fatal for the driver: it loads, plays nothing,
    // and its status says why.
    IpodPcm_Init();
    return(TRUE);
}
'@

# hwctxt.cpp, change 2
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
    m_OutputDeviceContext.SetBaseSampleRate(OUTPUT_SAMPLERATE);
'@ @'
    m_OutputDeviceContext.SetBaseSampleRate(IpodPcm_Rate());    // iPod: the codec's rate as found
'@

# hwctxt.cpp, change 3
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
    m_DMAOutPageVirtAddr[0] = pVirtDMABufferAddr;
    m_DMAOutPageVirtAddr[1] = pVirtDMABufferAddr + AUDIO_DMA_PAGE_SIZE;
'@ @'
    // iPod: the mixer fills these two 16-bit pages; TransferOutputBuffer
    // hands each finished page to the DMA controller's own memory
    static HWSAMPLE OutStage[2][IPODPCM_MAX_PAGE_FRAMES * OUTCHANNELS];
    m_DMAOutPageVirtAddr[0] = (PBYTE)OutStage[0];
    m_DMAOutPageVirtAddr[1] = (PBYTE)OutStage[1];
'@

# hwctxt.cpp, change 4
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
BOOL HardwareContext::InitOutputDMA()
{
    return TRUE;
}
'@ @'
BOOL HardwareContext::InitOutputDMA()
{
    return IpodPcm_Start();     // iPod
}
'@

# hwctxt.cpp, change 5
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
                goto Exit;
            }

            //----- 6. Start the DMA controller -----
'@ @'
                m_OutputDMARunning=FALSE;   // iPod: or no later attempt would be made
                goto Exit;
            }

            //----- 6. Start the DMA controller -----
'@

# hwctxt.cpp, change 6
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
        // Disable DMA
        ResetEvent(m_hOutputRunning);
'@ @'
        // Disable DMA
        IpodPcm_Stop();             // iPod
        ResetEvent(m_hOutputRunning);
'@

# hwctxt.cpp, change 7
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
    PBYTE pBufferStart = m_DMAOutPageVirtAddr[NumBuf];
    PBYTE pBufferEnd = pBufferStart + AUDIO_DMA_PAGE_SIZE;
'@ @'
    PBYTE pBufferStart = m_DMAOutPageVirtAddr[NumBuf];
    PBYTE pBufferEnd = pBufferStart + IpodPcm_PageFrames() * OUTCHANNELS * sizeof(HWSAMPLE);   // iPod
'@

# hwctxt.cpp, change 8
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
    SetSPDIFCompression(m_bPageHasEncodedData[0] || m_bPageHasEncodedData[1]);

    return BytesTransferred;
'@ @'
    SetSPDIFCompression(m_bPageHasEncodedData[0] || m_bPageHasEncodedData[1]);

    // iPod: the finished page goes to the DMA controller's memory
    IpodPcm_WritePage(NumBuf, (const short *)pBufferStart);

    return BytesTransferred;
'@

# hwctxt.cpp, change 9
$edited['hwctxt.cpp'] = Edit 'hwctxt.cpp' $edited['hwctxt.cpp'] @'
        int TimeDelta = m_TimeNextOutputInterrupt - GetTickCount();
        m_TimeNextOutputInterrupt += TIMEBETWEENINTERRUPTS;
        if (TimeDelta>0)
        {
            Sleep(TimeDelta);
        }

        Lock();

        ULONG OutputTransferred = TransferOutputBuffer(m_OutputDMABuffer);
'@ @'
        // iPod: the codec is the clock. A page is refilled when the DMA
        // controller has left it, not when a timer says so.
        Sleep(IPODPCM_POLL_MS);

        Lock();

        if (!m_OutputDMARunning || !IpodPcm_PageFinished(m_OutputDMABuffer))
        {
            Unlock();
            continue;
        }

        ULONG OutputTransferred = TransferOutputBuffer(m_OutputDMABuffer);
'@

# hwctxt.h, change 10
$edited['hwctxt.h'] = Edit 'hwctxt.h' $edited['hwctxt.h'] @'
#include "DtpNotify.h"
'@ @'
#include "DtpNotify.h"
#include <ipodpcm.h>        // iPod: the audio path
'@

# wavemain.cpp, change 11
$edited['wavemain.cpp'] = Edit 'wavemain.cpp' $edited['wavemain.cpp'] @'
                   PDWORD pdwActualOut)
{
    switch (dwCode)
    {
    case IOCTL_MIX_MESSAGE:
'@ @'
                   PDWORD pdwActualOut)
{
    // iPod: the status of the audio path, for the sound panel
    if (dwCode == IOCTL_IPODPCM_GET_STATUS)
    {
        return IpodPcm_StatusIoctl(pBufOut, dwLenOut, pdwActualOut);
    }

    switch (dwCode)
    {
    case IOCTL_MIX_MESSAGE:
'@

# all fitted: now the files are written
foreach ($n in $asIs) { Copy-Item -Force (Join-Path $src $n) (Join-Path $dest $n) }
foreach ($n in $edited.Keys)
{
    [IO.File]::WriteAllBytes((Join-Path $dest $n), $latin1.GetBytes($edited[$n].Replace("`n", "`r`n")))
}
[IO.File]::WriteAllText((Join-Path $dest 'getms.done'), "done`r`n")
Write-Host "getms.ps1: Microsoft's sample copied from $src, $($asIs.Count) files as they are, 3 changed"
[Environment]::Exit(0)
