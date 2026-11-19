#
# mkrbipod.ps1 - wrap a flat binary (nk.nb0) for the Rockbox iPod classic
#                bootloader (same format as "tools/scramble -add=ip6g").
#
# Output layout:
#   bytes 0-3 : big-endian checksum = (71 + sum of all payload bytes) mod 2^32
#               (71 = MODEL_NUMBER for ipod6g)
#   bytes 4-7 : "ip6g" (not checked by the bootloader)
#   bytes 8.. : payload, loaded to PA 0x08000000 and jumped to at offset 0
#
# Load limit: the stock Rockbox bootloader accepts payloads up to 8 MiB; the
# CE-on-iPod bootloader build (MAX_LOADSIZE 32M for IPOD_6G) up to 32 MiB.
# This script enforces 32 MiB and warns above 8 MiB.
#
# Trimming: romimage pads nk.nb0 to ROMSIZE (the whole NK region). The real
# image ends at ROMHDR.physlast, found through the ROM signature at offset
# 0x40 ('CECE', followed by the ROMHDR virtual address). Only that part is
# written, so the bootloader does not read megabytes of padding from disk.
# The memory past the image becomes RAM (AUTOSIZE) and the kernel does not
# rely on its contents. -NoTrim writes the whole file as before.
#
# Usage:
#   mkrbipod.ps1 -InFile nk.nb0 -OutFile wince.ipod [-NoTrim]
#   mkrbipod.ps1 -Verify -InFile wince.ipod
#
# Must run on Windows PowerShell 2.0 (Windows 7 build VM).
#

param(
    [Parameter(Mandatory=$true)] [string] $InFile,
    [string] $OutFile,
    [switch] $Verify,
    [switch] $NoTrim
)

$ErrorActionPreference = "Stop"

$ModelNumber  = 71
$ModelName    = "ip6g"
$MaxPayload   = 32 * 1024 * 1024    # CE-on-iPod bootloader (MAX_LOADSIZE 32M)
$StockPayload = 8 * 1024 * 1024     # stock Rockbox bootloader
$ImageBase    = [uint32]2147483648  # 0x80000000 = ROMSTART in config.bib (a hex literal would parse as a negative Int32)

# All bit manipulation lives in C#: PowerShell 2.0 has no -shl/-shr.
$code = @"
public static class RbIpod
{
    public static uint Sum(byte[] buf, int offset, int count, uint seed)
    {
        uint sum = seed;
        int end = offset + count;
        for (int i = offset; i < end; i++)
        {
            unchecked { sum += buf[i]; }
        }
        return sum;
    }

    public static uint ReadBE32(byte[] buf, int offset)
    {
        return ((uint)buf[offset] << 24) | ((uint)buf[offset + 1] << 16) |
               ((uint)buf[offset + 2] << 8) | (uint)buf[offset + 3];
    }

    public static uint ReadLE32(byte[] buf, int offset)
    {
        if (offset < 0 || buf.Length < offset + 4) return 0;
        return System.BitConverter.ToUInt32(buf, offset);
    }

    public static byte[] Header(uint sum, string model)
    {
        byte[] hdr = new byte[8];
        hdr[0] = (byte)((sum >> 24) & 0xFF);
        hdr[1] = (byte)((sum >> 16) & 0xFF);
        hdr[2] = (byte)((sum >> 8) & 0xFF);
        hdr[3] = (byte)(sum & 0xFF);
        byte[] name = System.Text.Encoding.ASCII.GetBytes(model);
        System.Array.Copy(name, 0, hdr, 4, 4);
        return hdr;
    }

    // Length of the CE image (ROMHDR.physlast - physfirst, rounded up to 4)
    // in a flat .nb0 that starts at 'imageBase'. Negative value = not found:
    //   -1 file too short, -2 no 'CECE' signature at 0x40,
    //   -3 ROMHDR pointer outside the file, -4 physfirst/physlast invalid,
    //   -5 image longer than the file
    public static long ImageLength(byte[] buf, uint imageBase)
    {
        const int SIG_OFFSET = 0x40;
        const uint ROM_SIGNATURE = 0x43454345;

        if (buf.Length < SIG_OFFSET + 8) return -1;
        if (ReadLE32(buf, SIG_OFFSET) != ROM_SIGNATURE) return -2;

        long hdrOffset = (long)ReadLE32(buf, SIG_OFFSET + 4) - imageBase;
        if (hdrOffset < 0 || hdrOffset + 16 > buf.Length) return -3;

        uint physfirst = ReadLE32(buf, (int)hdrOffset + 8);
        uint physlast  = ReadLE32(buf, (int)hdrOffset + 12);
        if (physfirst != imageBase || physlast <= physfirst) return -4;

        long len = ((long)physlast - physfirst + 3) & ~3L;
        if (len > buf.Length) return -5;
        return len;
    }
}
"@
Add-Type -TypeDefinition $code -Language CSharp

function Get-FullPath([string] $p) {
    if ([System.IO.Path]::IsPathRooted($p)) { return $p }
    return [System.IO.Path]::Combine((Get-Location).Path, $p)
}

$inPath = Get-FullPath $InFile
if (-not (Test-Path -LiteralPath $inPath)) {
    Write-Host "mkrbipod: input file not found: $inPath"
    [Environment]::Exit(1)
}
$data = [System.IO.File]::ReadAllBytes($inPath)

if ($Verify) {
    if ($data.Length -lt 8) {
        Write-Host "mkrbipod: file too short"
        [Environment]::Exit(1)
    }
    $stored = [RbIpod]::ReadBE32($data, 0)
    $name = [System.Text.Encoding]::ASCII.GetString($data, 4, 4)
    $len = $data.Length - 8
    $calc = [RbIpod]::Sum($data, 8, $len, [uint32]$ModelNumber)
    $first = [RbIpod]::ReadLE32($data, 8)
    Write-Host ("mkrbipod: model '{0}', payload {1} bytes, stored 0x{2:X8}, computed 0x{3:X8}, first word 0x{4:X8}" -f $name, $len, $stored, $calc, $first)
    if ($len -gt $MaxPayload) {
        Write-Host "mkrbipod: payload larger than 32 MiB, the bootloader will refuse it"
        [Environment]::Exit(1)
    }
    if ($len -gt $StockPayload) {
        Write-Host "mkrbipod: note: payload above 8 MiB needs the CE-on-iPod bootloader build"
    }
    if ($stored -ne $calc) {
        Write-Host "mkrbipod: CHECKSUM MISMATCH"
        [Environment]::Exit(1)
    }
    Write-Host "mkrbipod: OK"
    [Environment]::Exit(0)
}

if (-not $OutFile) {
    Write-Host "mkrbipod: -OutFile is required"
    [Environment]::Exit(1)
}

# Payload length: the real image end unless -NoTrim
$len = $data.Length
if (-not $NoTrim) {
    $imgLen = [RbIpod]::ImageLength($data, [uint32]$ImageBase)
    if ($imgLen -gt 0) {
        Write-Host ("mkrbipod: CE image is {0} bytes of {1} in {2} (ROMHDR physlast), trimming padding" -f $imgLen, $data.Length, $InFile)
        $len = [int]$imgLen
    }
    else {
        Write-Host ("mkrbipod: warning: no valid ROMHDR found (code {0}), writing the whole file" -f $imgLen)
    }
}

if ($len -gt $MaxPayload) {
    Write-Host ("mkrbipod: payload is {0} bytes, larger than the 32 MiB load limit" -f $len)
    [Environment]::Exit(1)
}
if ($len -gt $StockPayload) {
    Write-Host ("mkrbipod: note: payload is {0} bytes, above 8 MiB: needs the CE-on-iPod bootloader build" -f $len)
}

$sum = [RbIpod]::Sum($data, 0, $len, [uint32]$ModelNumber)
$hdr = [RbIpod]::Header($sum, $ModelName)

$outPath = Get-FullPath $OutFile
$fs = [System.IO.File]::Open($outPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
try {
    $fs.Write($hdr, 0, 8)
    $fs.Write($data, 0, $len)
}
finally {
    $fs.Close()
}

$first = [RbIpod]::ReadLE32($data, 0)
Write-Host ("mkrbipod: wrote {0} ({1} byte payload, checksum 0x{2:X8}, first word 0x{3:X8})" -f $OutFile, $len, $sum, $first)
[Environment]::Exit(0)
