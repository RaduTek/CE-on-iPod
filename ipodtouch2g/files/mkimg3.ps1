#
# mkimg3.ps1 - wrap a flat binary (nk.nb0) in an IMG3 container (type iBEC)
#              for a patched iBoot, started with loadibec.
#
# IMG3 layout (all fields little-endian):
#   header (20 bytes): magic "Img3", full size, size of the tags, size of the
#                      signed area (the tags before SHSH), ident ("ibec")
#   tags: magic, total length, data length, data, padding. The four-letter
#         names are stored as little-endian numbers, so "DATA" appears as
#         "ATAD" in a hex dump.
#
# With -Template (openiBoot's mk8900image\template-ipt2g.img3) every tag of
# the template is kept and only DATA is replaced by the payload, as
# openiBoot's build does. The SHSH/CERT tags then no longer match the data;
# only a patched iBoot accepts the result. Without -Template a minimal
# container (TYPE and DATA) is written.
#
# Trimming: romimage pads nk.nb0 to ROMSIZE (the whole NK region). The real
# image ends at ROMHDR.physlast, found through the ROM signature at offset
# 0x40 ('CECE', followed by the ROMHDR virtual address). Only that part is
# written. -NoTrim writes the whole file.
#
# Compression (-Compress): iBSS receives the upload in a 16 MiB USB buffer
# (tested 2026-11-28: a 12 MiB payload starts, 16 MiB resets into iOS), so
# larger images are packed. The payload then is
#   bytes 0 - 0xFFFF : the image as is (jump page, startup.s code and its
#                      strings), except the pack header in the jump page:
#                      0x20 'LZCE' (0x45435A4C), 0x24 packed length,
#                      0x28 head length (0x10000), 0x2C unpacked length
#                      (romimage leaves 0x04-0x3F zero; the script checks)
#   then             : the rest of the image in LZ4 block format (literal
#                      and match lengths in the token, 2-byte offsets)
# startup.s copies the packed data to PA 0x0E000000, copies the head to PA
# 0x08000000, unpacks the rest behind it and clears the pack header. The
# script unpacks its own output again and compares it before writing.
#
# Usage:
#   mkimg3.ps1 -InFile nk.nb0 -OutFile wince.img3 [-RawFile wince.bin]
#              [-Template template-ipt2g.img3] [-Compress] [-NoTrim]
#   mkimg3.ps1 -Verify -InFile wince.img3
#
# wince.bin (-RawFile) is the same payload as the DATA tag (packed or not).
#
# Must run on Windows PowerShell 2.0 (Windows 7 build VM).
#

param(
    [Parameter(Mandatory=$true)] [string] $InFile,
    [string] $OutFile,
    [string] $RawFile,
    [string] $Template,
    [switch] $Verify,
    [switch] $Compress,
    [switch] $NoTrim
)

$ErrorActionPreference = "Stop"

$MaxPayload = 32 * 1024 * 1024          # NKLEN in config.bib
$MaxUpload  = 16 * 1024 * 1024          # iBSS USB buffer: the whole IMG3 file must fit
$PackHead   = 0x10000                   # bytes left unpacked at the start (startup.s runs from here)
$ImageBase  = [uint32]2147483648        # 0x80000000 = ROMSTART in config.bib (a hex literal would parse as a negative Int32)

# All bit manipulation lives in C#: PowerShell 2.0 has no -shl/-shr.
# C# 2.0 only (no var, no lambdas): PowerShell 2.0 compiles with it.
$code = @"
using System;
using System.Collections.Generic;
using System.Text;

public static class Img3
{
    public const uint MAGIC      = 0x496D6733;  // "Img3"
    public const uint TAG_TYPE   = 0x54595045;  // "TYPE"
    public const uint TAG_DATA   = 0x44415441;  // "DATA"
    public const uint TAG_SHSH   = 0x53485348;  // "SHSH"
    public const uint TAG_KBAG   = 0x4B424147;  // "KBAG"
    public const uint IDENT_IBEC = 0x69626563;  // "ibec"

    public static uint ReadLE32(byte[] buf, int offset)
    {
        if (offset < 0 || buf.Length < offset + 4) return 0;
        return BitConverter.ToUInt32(buf, offset);
    }

    static void PutLE32(byte[] buf, int offset, uint value)
    {
        buf[offset]     = (byte)(value & 0xFF);
        buf[offset + 1] = (byte)((value >> 8) & 0xFF);
        buf[offset + 2] = (byte)((value >> 16) & 0xFF);
        buf[offset + 3] = (byte)((value >> 24) & 0xFF);
    }

    public static string FourCC(uint value)
    {
        char[] c = new char[4];
        c[0] = (char)((value >> 24) & 0xFF);
        c[1] = (char)((value >> 16) & 0xFF);
        c[2] = (char)((value >> 8) & 0xFF);
        c[3] = (char)(value & 0xFF);
        for (int i = 0; i < 4; i++)
        {
            if (c[i] < 0x20 || c[i] > 0x7E) c[i] = '?';
        }
        return new string(c);
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

    //--------------------------------------------------------------------
    // Packing (LZ4 block format, hash chains, greedy). Pack header words in
    // the jump page; see the comment at the top of the script.
    //--------------------------------------------------------------------
    public const uint PACK_MAGIC   = 0x45435A4C;    // "LZCE"
    public const int  PACK_HDR     = 0x20;          // offset of the pack header
    const int HASH_BITS = 16;
    const int CHAIN     = 128;                      // positions searched per hash
    const int MAX_DIST  = 65535;

    static int HashAt(byte[] b, int i)
    {
        uint v = BitConverter.ToUInt32(b, i);
        return (int)((v * 2654435761u) >> (32 - HASH_BITS));
    }

    static void PutLength(byte[] dst, ref int op, int n)
    {
        while (n >= 255) { dst[op++] = 255; n -= 255; }
        dst[op++] = (byte)n;
    }

    // LZ4 block of src[start .. start+len)
    public static byte[] Lz4Compress(byte[] src, int start, int len)
    {
        int end = start + len;
        int mflimit = end - 12;             // no match starts in the last 12 bytes
        int matchEnd = end - 5;             // the last 5 bytes are literals
        byte[] dst = new byte[len + len / 255 + 64];
        int[] head = new int[1 << HASH_BITS];
        int[] prev = new int[len];
        int op = 0, ip = start, anchor = start, i;

        for (i = 0; i < head.Length; i++) head[i] = -1;

        while (ip < mflimit)
        {
            int h = HashAt(src, ip);
            uint seq = BitConverter.ToUInt32(src, ip);
            int best = -1, bestLen = 0, tries = CHAIN;
            int cand = head[h];

            while (cand >= 0 && ip - cand <= MAX_DIST && tries-- > 0)
            {
                if (BitConverter.ToUInt32(src, cand) == seq)
                {
                    int l = 4;
                    while (ip + l < matchEnd && src[cand + l] == src[ip + l]) l++;
                    if (l > bestLen) { bestLen = l; best = cand; }
                }
                cand = prev[cand - start];
            }
            prev[ip - start] = head[h];
            head[h] = ip;

            if (best < 0) { ip++; continue; }

            int lit = ip - anchor;
            int ml = bestLen - 4;
            int tokenPos = op++;
            dst[tokenPos] = (byte)(((lit >= 15) ? 15 : lit) << 4 | ((ml >= 15) ? 15 : ml));
            if (lit >= 15) PutLength(dst, ref op, lit - 15);
            Array.Copy(src, anchor, dst, op, lit);
            op += lit;
            int off = ip - best;
            dst[op++] = (byte)(off & 0xFF);
            dst[op++] = (byte)((off >> 8) & 0xFF);
            if (ml >= 15) PutLength(dst, ref op, ml - 15);

            // index the positions inside the match
            for (int k = ip + 1; k < ip + bestLen && k < mflimit; k++)
            {
                int h2 = HashAt(src, k);
                prev[k - start] = head[h2];
                head[h2] = k;
            }
            ip += bestLen;
            anchor = ip;
        }

        // last literals
        {
            int lit = end - anchor;
            dst[op++] = (byte)(((lit >= 15) ? 15 : lit) << 4);
            if (lit >= 15) PutLength(dst, ref op, lit - 15);
            Array.Copy(src, anchor, dst, op, lit);
            op += lit;
        }

        byte[] result = new byte[op];
        Array.Copy(dst, result, op);
        return result;
    }

    // Reference unpacker, the same steps as the ARM code in startup.s.
    // Returns false on malformed input or a length mismatch.
    public static bool Lz4Decompress(byte[] src, int srcStart, int srcLen,
                                     byte[] dst, int dstStart, int dstLen)
    {
        int ip = srcStart, ipEnd = srcStart + srcLen;
        int op = dstStart, opEnd = dstStart + dstLen;

        while (ip < ipEnd)
        {
            int token = src[ip++];
            int lit = token >> 4;
            if (lit == 15)
            {
                int b;
                do { if (ip >= ipEnd) return false; b = src[ip++]; lit += b; } while (b == 255);
            }
            if (ip + lit > ipEnd || op + lit > opEnd) return false;
            Array.Copy(src, ip, dst, op, lit);
            ip += lit;
            op += lit;
            if (ip >= ipEnd) break;             // last sequence: literals only

            if (ip + 2 > ipEnd) return false;
            int off = src[ip] | (src[ip + 1] << 8);
            ip += 2;
            int ml = token & 15;
            if (ml == 15)
            {
                int b;
                do { if (ip >= ipEnd) return false; b = src[ip++]; ml += b; } while (b == 255);
            }
            ml += 4;
            if (off == 0 || op - off < dstStart || op + ml > opEnd) return false;
            for (int k = 0; k < ml; k++, op++) dst[op] = dst[op - off];   // may overlap
        }
        return op == opEnd;
    }

    // Packed payload of image[0 .. len); see the top of the script.
    public static byte[] Pack(byte[] image, int len, int headLen)
    {
        if (len <= headLen)
            throw new Exception("the image is not larger than the unpacked head");
        for (int i = 0x04; i < 0x40; i++)
        {
            if (image[i] != 0)
                throw new Exception("jump page byte 0x" + i.ToString("X2") + " is not zero, no room for the pack header");
        }

        byte[] packed = Lz4Compress(image, headLen, len - headLen);

        byte[] payload = new byte[headLen + packed.Length];
        Array.Copy(image, 0, payload, 0, headLen);
        Array.Copy(packed, 0, payload, headLen, packed.Length);
        PutLE32(payload, PACK_HDR + 0x0, PACK_MAGIC);
        PutLE32(payload, PACK_HDR + 0x4, (uint)packed.Length);
        PutLE32(payload, PACK_HDR + 0x8, (uint)headLen);
        PutLE32(payload, PACK_HDR + 0xC, (uint)len);

        // unpack again and compare (header words are zero in the unpacked image)
        byte[] check = new byte[len];
        Array.Copy(payload, 0, check, 0, headLen);
        for (int i = PACK_HDR; i < PACK_HDR + 0x10; i++) check[i] = 0;
        if (!Lz4Decompress(payload, headLen, packed.Length, check, headLen, len - headLen))
            throw new Exception("packing check failed: the packed data does not unpack");
        for (int i = 0; i < len; i++)
        {
            if (check[i] != image[i])
                throw new Exception("packing check failed at offset 0x" + i.ToString("X"));
        }
        return payload;
    }

    // One tag. totalLen 0 = header + data rounded up to 4 bytes.
    static byte[] MakeTag(uint magic, byte[] data, int count, int totalLen)
    {
        int minLen = (12 + count + 3) & ~3;
        if (totalLen < minLen) totalLen = minLen;
        byte[] tag = new byte[totalLen];
        PutLE32(tag, 0, magic);
        PutLE32(tag, 4, (uint)totalLen);
        PutLE32(tag, 8, (uint)count);
        Array.Copy(data, 0, tag, 12, count);
        return tag;
    }

    // template == null: minimal container (TYPE "ibec", DATA).
    public static byte[] Build(byte[] template, byte[] payload, int payloadLen)
    {
        List<byte[]> tags = new List<byte[]>();
        uint ident = IDENT_IBEC;

        if (template != null)
        {
            if (template.Length < 20 || ReadLE32(template, 0) != MAGIC)
                throw new Exception("the template is not an IMG3 file");
            ident = ReadLE32(template, 16);

            int end = 20 + (int)ReadLE32(template, 8);
            if (end > template.Length) end = template.Length;

            bool haveData = false;
            int o = 20;
            while (o + 12 <= end)
            {
                uint magic = ReadLE32(template, o);
                int total = (int)ReadLE32(template, o + 4);
                if (total < 12 || o + total > end)
                    throw new Exception("bad tag in the template at offset " + o);
                if (magic == TAG_KBAG)
                    throw new Exception("the template has a KBAG tag (encrypted data)");

                if (magic == TAG_DATA)
                {
                    tags.Add(MakeTag(TAG_DATA, payload, payloadLen, 0));
                    haveData = true;
                }
                else
                {
                    byte[] copy = new byte[total];
                    Array.Copy(template, o, copy, 0, total);
                    tags.Add(copy);
                }
                o += total;
            }
            if (!haveData)
                throw new Exception("the template has no DATA tag");
        }
        else
        {
            byte[] type = new byte[4];
            PutLE32(type, 0, IDENT_IBEC);
            tags.Add(MakeTag(TAG_TYPE, type, 4, 32));     // 32 bytes as in Apple's files
            tags.Add(MakeTag(TAG_DATA, payload, payloadLen, 0));
        }

        int size = 0;
        int signedArea = -1;
        foreach (byte[] tag in tags)
        {
            if (signedArea < 0 && ReadLE32(tag, 0) == TAG_SHSH) signedArea = size;
            size += tag.Length;
        }
        if (signedArea < 0) signedArea = size;

        byte[] img = new byte[20 + size];
        PutLE32(img, 0, MAGIC);
        PutLE32(img, 4, (uint)(20 + size));
        PutLE32(img, 8, (uint)size);
        PutLE32(img, 12, (uint)signedArea);
        PutLE32(img, 16, ident);
        int p = 20;
        foreach (byte[] tag in tags)
        {
            Array.Copy(tag, 0, img, p, tag.Length);
            p += tag.Length;
        }
        return img;
    }

    // Text description of an IMG3 file; sets ok = false on a format error.
    public static string Describe(byte[] img, out bool ok)
    {
        StringBuilder sb = new StringBuilder();
        ok = false;
        if (img.Length < 20 || ReadLE32(img, 0) != MAGIC)
        {
            sb.Append("not an IMG3 file\r\n");
            return sb.ToString();
        }
        uint full = ReadLE32(img, 4);
        uint tagsSize = ReadLE32(img, 8);
        sb.AppendFormat("header: full size {0} (file {1}), tags {2}, signed area {3}, ident '{4}'\r\n",
            full, img.Length, tagsSize, ReadLE32(img, 12), FourCC(ReadLE32(img, 16)));
        if (full != img.Length || tagsSize + 20 != full)
        {
            sb.Append("sizes in the header do not match the file\r\n");
            return sb.ToString();
        }

        bool haveData = false;
        int o = 20;
        while (o + 12 <= img.Length)
        {
            uint magic = ReadLE32(img, o);
            int total = (int)ReadLE32(img, o + 4);
            int count = (int)ReadLE32(img, o + 8);
            if (total < 12 || o + total > img.Length || count > total - 12)
            {
                sb.AppendFormat("bad tag at offset {0}\r\n", o);
                return sb.ToString();
            }
            sb.AppendFormat("  tag '{0}' at {1}: total {2}, data {3}", FourCC(magic), o, total, count);
            if (magic == TAG_DATA)
            {
                haveData = true;
                sb.AppendFormat(", first word 0x{0:X8}", ReadLE32(img, o + 12));
                if (count >= 0x48 && ReadLE32(img, o + 12 + 0x40) == 0x43454345)
                    sb.AppendFormat(", CE image (ROMHDR at 0x{0:X8})", ReadLE32(img, o + 12 + 0x44));
                if (count >= 0x30 && ReadLE32(img, o + 12 + PACK_HDR) == PACK_MAGIC)
                    sb.AppendFormat(", packed: head {0}, packed {1}, unpacked {2}",
                        ReadLE32(img, o + 12 + PACK_HDR + 8), ReadLE32(img, o + 12 + PACK_HDR + 4),
                        ReadLE32(img, o + 12 + PACK_HDR + 12));
            }
            sb.Append("\r\n");
            o += total;
        }
        ok = haveData && (o == img.Length);
        if (!haveData) sb.Append("no DATA tag\r\n");
        return sb.ToString();
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
    Write-Host "mkimg3: input file not found: $inPath"
    [Environment]::Exit(1)
}
$data = [System.IO.File]::ReadAllBytes($inPath)

if ($Verify) {
    $ok = $false
    $text = [Img3]::Describe($data, [ref] $ok)
    Write-Host -NoNewline ("mkimg3: {0}`r`n{1}" -f $InFile, $text)
    if (-not $ok) {
        Write-Host "mkimg3: NOT OK"
        [Environment]::Exit(1)
    }
    Write-Host "mkimg3: OK"
    [Environment]::Exit(0)
}

if (-not $OutFile) {
    Write-Host "mkimg3: -OutFile is required"
    [Environment]::Exit(1)
}

# Payload length: the real image end unless -NoTrim
$len = $data.Length
if (-not $NoTrim) {
    $imgLen = [Img3]::ImageLength($data, [uint32]$ImageBase)
    if ($imgLen -gt 0) {
        Write-Host ("mkimg3: CE image is {0} bytes of {1} in {2} (ROMHDR physlast), trimming padding" -f $imgLen, $data.Length, $InFile)
        $len = [int]$imgLen
    }
    else {
        Write-Host ("mkimg3: warning: no valid ROMHDR found (code {0}), writing the whole file" -f $imgLen)
    }
}

if ($len -gt $MaxPayload) {
    Write-Host ("mkimg3: payload is {0} bytes, larger than the 32 MiB NK region" -f $len)
    [Environment]::Exit(1)
}

$tpl = $null
if ($Template) {
    $tplPath = Get-FullPath $Template
    if (-not (Test-Path -LiteralPath $tplPath)) {
        Write-Host "mkimg3: template not found: $tplPath"
        [Environment]::Exit(1)
    }
    $tpl = [System.IO.File]::ReadAllBytes($tplPath)
    Write-Host ("mkimg3: using template {0}" -f $tplPath)
}

# Payload: the image itself, or packed
$payload = $data
$plen = $len
if ($Compress) {
    try {
        $t0 = Get-Date
        $payload = [Img3]::Pack($data, $len, $PackHead)
        $plen = $payload.Length
        $secs = ((Get-Date) - $t0).TotalSeconds
        $pct = 100.0 * $plen / $len
        Write-Host ("mkimg3: packed {0} bytes to {1} ({2:N1}%, head {3} bytes), unpack check OK, {4:N1} s" -f $len, $plen, $pct, $PackHead, $secs)
    }
    catch {
        Write-Host ("mkimg3: {0}" -f $_.Exception.Message)
        [Environment]::Exit(1)
    }
}

try {
    $img = [Img3]::Build($tpl, $payload, $plen)
}
catch {
    Write-Host ("mkimg3: {0}" -f $_.Exception.Message)
    [Environment]::Exit(1)
}

if ($img.Length -gt $MaxUpload) {
    Write-Host ("mkimg3: the IMG3 file would be {0} bytes, larger than iBSS's 16 MiB upload buffer ({1})" -f $img.Length, $MaxUpload)
    if (-not $Compress) { Write-Host "mkimg3: use -Compress" }
    [Environment]::Exit(1)
}

$outPath = Get-FullPath $OutFile
[System.IO.File]::WriteAllBytes($outPath, $img)

if ($RawFile) {
    $rawPath = Get-FullPath $RawFile
    $fs = [System.IO.File]::Open($rawPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
    try {
        $fs.Write($payload, 0, $plen)
    }
    finally {
        $fs.Close()
    }
    Write-Host ("mkimg3: wrote {0} ({1} bytes)" -f $RawFile, $plen)
}

$first = [Img3]::ReadLE32($payload, 0)
Write-Host ("mkimg3: wrote {0} ({1} bytes, payload {2} bytes, first word 0x{3:X8})" -f $OutFile, $img.Length, $plen, $first)
[Environment]::Exit(0)
