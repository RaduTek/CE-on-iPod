//
// z2.c - Apple Zephyr2 multi-touch controller protocol (iPod touch 2G, N72)
//
// Written from the description of the protocol in n72-linux's
// drivers/input/touchscreen/zephyr2.c (GPL, used as documentation, not
// copied) and openiBoot's plat-s5l8900/multitouch-z2.c:
//
//   Boot (z2_boot): reset on 200 ms, a 16-byte dummy transfer (SCLK idles
//   high), reset off, 15 ms, the dummy transfer again (takes the "HBPP
//   ready banner" 1f 01: verified by the OAL probe), then the firmware in
//   one 4.5 MHz transfer, 300 us, short ack 0x4bc1. Calibration in 0x3f0-
//   byte "3001" packets to 0x400200, each acknowledged 0x4bc1. Calibrate:
//   read the version register 0x10008ffc (older controllers need four
//   register writes), send 1f 01, 65 ms, long ack. Execute packet, 1 ms,
//   interface version (e2), info reports (e3 size, e6/e7 data).
//
//   Frames (z2_read_frame): when data ready is low, query the length (ea or
//   eb with the alternating "nop" byte), then read length + 5 bytes at
//   2 MHz. The frame has a 24-byte header and 28 bytes per finger.
//
// Byte orders: 16-bit fields big-endian, 32-bit fields "middle-endian"
// (bytes 2 3 0 1), the frame length query's checksum little-endian.
//
#include <windows.h>

#include "z2hw.h"
#include "z2.h"

#define Z2_ACK_OK               0x4bc1
#define Z2_REG_ACK_OK           0x4ad1
#define Z2_CAL_ADDR             0x400200
#define Z2_CAL_CHUNK            0x3f0
#define Z2_BUF_SIZE             0x400

#define MT_INFO_FAMILYID        0xd1
#define MT_INFO_SENSORINFO      0xd3
#define MT_INFO_SENSORREGIONDESC 0xd0
#define MT_INFO_SENSORREGIONPARAM 0xa1
#define MT_INFO_SENSORDIM       0xd9

#define FRAME_HEADER_LEN        24
#define FINGER_LEN              28

typedef struct
{
    DWORD hz;
    DWORD txDelayUs;            // after a transfer whose answer does not matter
    DWORD rxDelayUs;            // after a transfer whose answer is read
} Z2_SPEED;

static const Z2_SPEED z2Normal = { 83000, 5000, 10000 };
static const Z2_SPEED z2Fast   = { 4500000, 0, 5000 };
// frame reads: Z2_STATE.frameHz, no delays (Linux: 2 MHz)

static BYTE g_ptx[Z2_BUF_SIZE + 0x10];
static BYTE g_prx[Z2_BUF_SIZE + 0x10];

//------------------------------------------------------------------------------
// Byte orders and sums
//
static void PutU16(BYTE *b, DWORD v)  { b[0] = (BYTE)(v >> 8); b[1] = (BYTE)v; }
static DWORD GetU16(const BYTE *b)    { return ((DWORD)b[0] << 8) | b[1]; }
static void PutU16r(BYTE *b, DWORD v) { b[1] = (BYTE)(v >> 8); b[0] = (BYTE)v; }

static void PutU32(BYTE *b, DWORD v)
{
    b[2] = (BYTE)(v >> 24);
    b[3] = (BYTE)(v >> 16);
    b[0] = (BYTE)(v >> 8);
    b[1] = (BYTE)v;
}

static DWORD GetU32(const BYTE *b)
{
    return ((DWORD)b[2] << 24) | ((DWORD)b[3] << 16) | ((DWORD)b[0] << 8) | b[1];
}

static DWORD GetLE16(const BYTE *b)   { return b[0] | ((DWORD)b[1] << 8); }
static DWORD GetLE32(const BYTE *b)
{
    return b[0] | ((DWORD)b[1] << 8) | ((DWORD)b[2] << 16) | ((DWORD)b[3] << 24);
}

static DWORD Sum(const BYTE *b, DWORD len)
{
    DWORD s = 0;

    while (len--)
        s += *b++;
    return s;
}

static void FormatBytes(const BYTE *p, DWORD len, WCHAR *pszOut, DWORD maxBytes)
{
    static const WCHAR hex[] = L"0123456789abcdef";
    DWORD i;

    if (len > maxBytes)
        len = maxBytes;
    for (i = 0; i < len; i++)
    {
        pszOut[i * 3]     = hex[p[i] >> 4];
        pszOut[i * 3 + 1] = hex[p[i] & 0xF];
        pszOut[i * 3 + 2] = L' ';
    }
    pszOut[(len > 0) ? (len * 3 - 1) : 0] = L'\0';
}

//------------------------------------------------------------------------------
// Transfers and acknowledgements
//
static BOOL Xfer(const Z2_SPEED *sp, const BYTE *pTx, BYTE *pRx, DWORD len, BOOL fRx)
{
    BOOL ok = Z2Hw_Xfer(pTx, pRx, len, sp->hz, fRx ? sp->rxDelayUs : sp->txDelayUs);

    if (!ok)
        RETAILMSG(1, (L"ipttouch: SPI timeout (%d bytes at %d Hz, status %08x)\r\n",
                      len, sp->hz, Z2Hw_LastStatus()));
    return ok;
}

static DWORD ShortAck(void)
{
    BYTE tx[2], rx[2] = { 0, 0 };

    PutU16(tx, 0x1aa1);
    if (!Xfer(&z2Normal, tx, rx, 2, TRUE))
        return 0;
    return GetU16(rx);
}

static DWORD LongAck(void)
{
    BYTE tx[8], rx[8];

    memset(rx, 0, sizeof(rx));
    PutU16(tx, 0x1aa1);
    PutU16(tx + 2, 0x18e1);
    PutU16(tx + 4, 0x18e1);
    PutU16(tx + 6, 0x18e1);
    if (!Xfer(&z2Normal, tx, rx, 8, TRUE))
        return 0;
    return GetU32(rx + 2);
}

static DWORD ReadRegister(DWORD addr)
{
    BYTE tx[8];

    PutU16(tx, 0x1c73);
    PutU32(tx + 2, addr);
    PutU16(tx + 6, Sum(tx + 2, 4));
    Xfer(&z2Normal, tx, NULL, 8, TRUE);
    return LongAck();
}

static BOOL WriteRegister(DWORD addr, DWORD value, DWORD mask)
{
    BYTE tx[16];

    PutU16(tx, 0x1e33);
    PutU32(tx + 2, addr);
    PutU32(tx + 6, mask);
    PutU32(tx + 10, value);
    PutU16(tx + 14, Sum(tx + 2, 12));
    Xfer(&z2Normal, tx, NULL, 16, TRUE);
    return ShortAck() == Z2_REG_ACK_OK;
}

//------------------------------------------------------------------------------
// Firmware and calibration
//
static BOOL LoadFirmware(const BYTE *pFw, DWORD fwLen)
{
    HANDLE hThread = GetCurrentThread();
    int oldPrio = CeGetThreadPriority(hThread);
    DWORD ack, t0;
    int tries;
    BOOL ok;

    for (tries = 0; tries < 5; tries++)
    {
        // keep the FIFO fed during the 12 ms transfer
        CeSetThreadPriority(hThread, 100);
        t0 = GetTickCount();
        ok = Z2Hw_Xfer(pFw, NULL, fwLen, z2Fast.hz, z2Fast.txDelayUs);
        CeSetThreadPriority(hThread, oldPrio);
        if (!ok)
        {
            RETAILMSG(1, (L"ipttouch: firmware transfer timed out (status %08x)\r\n",
                          Z2Hw_LastStatus()));
            continue;
        }
        Z2Hw_DelayUs(300);
        ack = ShortAck();
        RETAILMSG(1, (L"ipttouch: firmware %d bytes sent in %d ms, ack 0x%04x%s\r\n",
                      fwLen, GetTickCount() - t0, ack, (ack == Z2_ACK_OK) ? L" (ok)" : L""));
        if (ack == Z2_ACK_OK)
            return TRUE;
    }
    return FALSE;
}

// 18 e1, then a "3001" bootloader packet with "middle-endian" data
static DWORD MakePacket(BYTE *pOut, DWORD addr, const BYTE *pData, DWORD len)
{
    DWORD i;

    pOut[0] = 0x18;
    pOut[1] = 0xe1;
    pOut[2] = 0x30;
    pOut[3] = 0x01;
    PutU16(pOut + 4, len >> 2);
    PutU32(pOut + 6, addr);
    PutU16(pOut + 10, Sum(pOut + 4, 6));
    for (i = 0; i < len; i += 4)
    {
        pOut[12 + i + 0] = pData[i + 1];
        pOut[12 + i + 1] = pData[i + 0];
        pOut[12 + i + 2] = pData[i + 3];
        pOut[12 + i + 3] = pData[i + 2];
    }
    PutU32(pOut + 12 + len, Sum(pOut + 12, len));
    return len + 0x10;
}

static BOOL LoadCal(const BYTE *pCal, DWORD calLen)
{
    DWORD off = 0, addr = Z2_CAL_ADDR, raw, n, len, ack = 0;
    int tries;

    while (off < calLen)
    {
        raw = calLen - off;
        if (raw > Z2_CAL_CHUNK)
            raw = Z2_CAL_CHUNK;
        n = (raw + 3) & ~3;

        memset(g_prx, 0, n);
        memcpy(g_prx, pCal + off, raw);
        len = MakePacket(g_ptx, addr, g_prx, n);
        for (tries = 0; tries < 5; tries++)
        {
            if (!Xfer(&z2Fast, g_ptx, NULL, len, FALSE))
                continue;
            Z2Hw_DelayUs(300);
            ack = ShortAck();
            if (ack == Z2_ACK_OK)
                break;
        }
        RETAILMSG(1, (L"ipttouch: calibration %d bytes to 0x%06x, ack 0x%04x after %d tries\r\n",
                      raw, addr, ack, tries + 1));
        if (tries == 5)
            return FALSE;
        addr += n;
        off += raw;
    }
    return TRUE;
}

static BOOL Calibrate(Z2_STATE *pZ2)
{
    BYTE tx[2] = { 0x1f, 0x01 };
    DWORD ack;

    pZ2->version = ReadRegister(0x10008ffc);
    RETAILMSG(1, (L"ipttouch: Zephyr2 version 0x%08x\r\n", pZ2->version));
    if (pZ2->version != 0x5a020028 && pZ2->version != 0x5a02002a)
    {
        if (!WriteRegister(0x10001c04, 0x16e4, 0x1fff) ||
            !WriteRegister(0x10001c08, 0x840000, 0xff0000) ||
            !WriteRegister(0x1000300c, 0x05, 0x85) ||
            !WriteRegister(0x1000304c, 0x20, 0xffffffff))
        {
            RETAILMSG(1, (L"ipttouch: register initialisation failed\r\n"));
            return FALSE;
        }
        RETAILMSG(1, (L"ipttouch: register initialisation done (older controller)\r\n"));
    }

    Xfer(&z2Normal, tx, NULL, 2, TRUE);
    Sleep(65);
    ack = LongAck();
    RETAILMSG(1, (L"ipttouch: calibration done, ack 0x%08x\r\n", ack));
    return TRUE;
}

static void SendExecute(void)
{
    BYTE tx[12] = { 0x1d, 0x53, 0x18, 0x00, 0x10, 0x00, 0x00, 0x01, 0x00, 0x00, 0, 0 };

    PutU16(tx + 10, Sum(tx + 2, 8));
    Xfer(&z2Normal, tx, NULL, 12, TRUE);
}

//------------------------------------------------------------------------------
// Commands and reports
//
// 16-byte command frame with the sum of bytes 0-13 in bytes 14-15 (LE)
static void Cmd16(BYTE *pTx, BYTE b0, BYTE b1, BYTE b2, DWORD b34)
{
    DWORD s;

    memset(pTx, 0, 16);
    pTx[0] = b0;
    pTx[1] = b1;
    pTx[2] = b2;
    pTx[3] = (BYTE)b34;
    pTx[4] = (BYTE)(b34 >> 8);
    s = Sum(pTx, 14);
    pTx[14] = (BYTE)s;
    pTx[15] = (BYTE)(s >> 8);
}

static BOOL Rx16Ok(const BYTE *pRx, BYTE expect)
{
    return pRx[0] == expect && GetLE16(pRx + 14) == (Sum(pRx, 14) & 0xffff);
}

static BOOL InterfaceVersion(Z2_STATE *pZ2)
{
    BYTE tx[16], rx[16];
    int tries;

    // the answer comes in the next exchange: send once and discard
    Cmd16(tx, 0xe2, 0, 0, 0);
    Xfer(&z2Normal, tx, rx, 16, TRUE);
    for (tries = 0; tries < 10; tries++)
    {
        memset(rx, 0, sizeof(rx));
        Xfer(&z2Normal, tx, rx, 16, TRUE);
        if (Rx16Ok(rx, 0xe2))
        {
            pZ2->interfaceVersion = rx[2];
            pZ2->maxPacket = (rx[4] << 8) | rx[3];
            return TRUE;
        }
        Sleep(5);
    }
    pZ2->maxPacket = 1000;
    return FALSE;
}

static BOOL GetReport(int id, BYTE *pBuf, int bufSize, int *pOutLen)
{
    BYTE tx[16], rx[16];
    int tries, len = 0, copy;
    DWORD s;

    memset(rx, 0, sizeof(rx));
    for (tries = 0; tries < 4; tries++)
    {
        Cmd16(tx, 0xe3, (BYTE)id, 0, 0);
        if (!Xfer(&z2Normal, tx, rx, 16, TRUE))
            continue;
        if (Rx16Ok(rx, 0xe3) && rx[2] == 0)
        {
            len = (rx[4] << 8) | rx[3];
            break;
        }
    }
    if (tries == 4 || len <= 0 || len > Z2_BUF_SIZE - 8)
        return FALSE;

    memset(g_ptx, 0, Z2_BUF_SIZE);
    copy = (len < bufSize) ? len : bufSize;

    for (tries = 0; tries < 4; tries++)
    {
        if (len < 12)
        {
            Cmd16(g_ptx, 0xe6, (BYTE)id, 0, len);
            if (!Xfer(&z2Normal, g_ptx, NULL, 16, TRUE))
                continue;
            Z2Hw_DelayUs(25);
            g_ptx[2] = 1;               // same frame, byte 2 = 1, checksum kept
            if (!Xfer(&z2Normal, g_ptx, g_prx, 16, TRUE))
                continue;
            if (GetLE16(g_prx + 14) == (Sum(g_prx, 14) & 0xffff))
            {
                memcpy(pBuf, g_prx + 3, copy);
                *pOutLen = copy;
                return TRUE;
            }
        }
        else
        {
            Cmd16(g_ptx, 0xe7, (BYTE)id, 0, len);
            s = Sum(g_ptx, 5);
            if (!Xfer(&z2Normal, g_ptx, NULL, 16, TRUE))
                continue;
            g_ptx[2] = 1;
            g_ptx[14] = 0;
            g_ptx[15] = 0;
            g_ptx[3 + len] = (BYTE)s;
            g_ptx[3 + len + 1] = (BYTE)(s >> 8);
            if (!Xfer(&z2Normal, g_ptx, g_prx, len + 5, TRUE))
                continue;
            s = Sum(g_prx, len + 3);
            if (GetLE16(g_prx + 3 + len) == (s & 0xffff))
            {
                memcpy(pBuf, g_prx + 3, copy);
                *pOutLen = copy;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void LogReport(const WCHAR *pszName, int id, BYTE *buf, int bufSize, int *pLen)
{
    WCHAR line[32 * 3 + 1];

    *pLen = 0;
    if (GetReport(id, buf, bufSize, pLen))
    {
        FormatBytes(buf, *pLen, line, 32);
        RETAILMSG(1, (L"ipttouch: report 0x%02x (%s): %d bytes: %s\r\n", id, pszName, *pLen, line));
    }
    else
    {
        RETAILMSG(1, (L"ipttouch: report 0x%02x (%s) failed\r\n", id, pszName));
    }
}

//------------------------------------------------------------------------------
// Boot
//
static BOOL BootOnce(Z2_STATE *pZ2, const BYTE *pFw, DWORD fwLen, const BYTE *pCal, DWORD calLen)
{
    static const BYTE prime[16] = {
        0x1a, 0xa1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
        0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1, 0x18, 0xe1,
    };
    BYTE rx[16], buf[32];
    WCHAR line[16 * 3 + 1];
    int i, len;

    for (i = 0; i < 4; i++)
    {
        Z2Hw_Reset(TRUE);
        Sleep(200);
        Xfer(&z2Normal, prime, NULL, sizeof(prime), FALSE);
        Z2Hw_Reset(FALSE);
        Sleep(15);
        memset(rx, 0, sizeof(rx));
        Xfer(&z2Normal, prime, rx, sizeof(prime), FALSE);
        FormatBytes(rx, sizeof(rx), line, 16);
        RETAILMSG(1, (L"ipttouch: reset %d: banner %s, data ready %d\r\n", i + 1, line, Z2Hw_DataReady()));
        if (LoadFirmware(pFw, fwLen))
            break;
    }
    if (i == 4)
    {
        RETAILMSG(1, (L"ipttouch: firmware upload failed\r\n"));
        return FALSE;
    }

    if (!LoadCal(pCal, calLen))
    {
        RETAILMSG(1, (L"ipttouch: calibration upload failed\r\n"));
        return FALSE;
    }
    if (!Calibrate(pZ2))
        return FALSE;
    SendExecute();
    Sleep(1);

    if (!InterfaceVersion(pZ2))
    {
        RETAILMSG(1, (L"ipttouch: no interface version\r\n"));
        return FALSE;
    }
    RETAILMSG(1, (L"ipttouch: interface version %d, max packet %d\r\n",
                  pZ2->interfaceVersion, pZ2->maxPacket));

    LogReport(L"family id", MT_INFO_FAMILYID, buf, sizeof(buf), &len);
    if (len >= 1)
        pZ2->familyId = buf[0];
    LogReport(L"sensor info", MT_INFO_SENSORINFO, buf, sizeof(buf), &len);
    if (len >= 5)
    {
        pZ2->rows = buf[1];
        pZ2->columns = buf[2];
        pZ2->bcdVersion = (buf[3] << 8) | buf[4];
    }
    LogReport(L"sensor dimensions", MT_INFO_SENSORDIM, buf, sizeof(buf), &len);
    if (len >= 8)
    {
        pZ2->sensorWidth = (9000 - (int)GetLE32(buf)) * 84 / 73;
        pZ2->sensorHeight = (13850 - (int)GetLE32(buf + 4)) * 84 / 73;
    }
    if (pZ2->sensorWidth <= 0 || pZ2->sensorHeight <= 0)
    {
        pZ2->sensorWidth = 5000;        // iOS: surface 5000 x 7500
        pZ2->sensorHeight = 7500;
    }
    LogReport(L"sensor region descriptor", MT_INFO_SENSORREGIONDESC, buf, sizeof(buf), &len);
    LogReport(L"sensor region parameters", MT_INFO_SENSORREGIONPARAM, buf, sizeof(buf), &len);

    RETAILMSG(1, (L"ipttouch: family 0x%02x (%d), sensor %d rows x %d columns, bcd 0x%04x, surface %d x %d\r\n",
                  pZ2->familyId, pZ2->familyId, pZ2->rows, pZ2->columns, pZ2->bcdVersion,
                  pZ2->sensorWidth, pZ2->sensorHeight));

    pZ2->flipNop = (pZ2->bcdVersion > 0x23);
    pZ2->curNop = 1;
    pZ2->lastFrameOk = TRUE;
    return TRUE;
}

BOOL Z2_Boot(Z2_STATE *pZ2, const BYTE *pFw, DWORD fwLen, const BYTE *pCal, DWORD calLen,
             DWORD frameHz)
{
    int tries;

    memset(pZ2, 0, sizeof(*pZ2));
    pZ2->frameHz = (frameHz != 0) ? frameHz : 2000000;
    for (tries = 1; tries <= 3; tries++)
    {
        if (BootOnce(pZ2, pFw, fwLen, pCal, calLen))
            return TRUE;
        RETAILMSG(1, (L"ipttouch: start %d failed%s\r\n", tries, (tries < 3) ? L", trying again" : L""));
        Sleep(100);
    }
    Z2Hw_Reset(TRUE);
    return FALSE;
}

//------------------------------------------------------------------------------
// Frames
//
const WCHAR *Z2_ErrName(int step)
{
    switch (step)
    {
    case Z2_ERR_QUERY_XFER:     return L"length query: SPI timeout";
    case Z2_ERR_QUERY_TYPE:     return L"length query: answer type not 0xeN";
    case Z2_ERR_QUERY_SUM:      return L"length query: checksum";
    case Z2_ERR_QUERY_LEN:      return L"length query: length > max packet";
    case Z2_ERR_FRAME_XFER:     return L"frame: SPI timeout";
    case Z2_ERR_FRAME_TYPE:     return L"frame: type not 0xea/0xeb";
    case Z2_ERR_FRAME_HEADSUM:  return L"frame: header sum";
    case Z2_ERR_FRAME_PLEN:     return L"frame: payload length";
    case Z2_ERR_FRAME_SUM:      return L"frame: payload checksum";
    default:                    return L"none";
    }
}

// Record a failed check and the first 16 bytes of the transfer
static void SetErr(Z2_STATE *pZ2, int step, int value, const BYTE *pTx, const BYTE *pRx, int len)
{
    int n = (len < 16) ? len : 16;

    pZ2->errStep = step;
    pZ2->errValue = value;
    pZ2->errLen = len;
    memset(pZ2->errTx, 0, sizeof(pZ2->errTx));
    memset(pZ2->errRx, 0, sizeof(pZ2->errRx));
    if (n > 0)
    {
        memcpy(pZ2->errTx, pTx, n);
        memcpy(pZ2->errRx, pRx, n);
    }
}

// The controller sends its answers as one byte stream (length reply, then
// the frame). Sometimes, while a finger is down, a transfer starts with a
// filler byte and the real answer follows one byte late; the transfer's
// last byte is then still waiting in the controller (seen 2026-11-28).
// Read that one byte and drop the filler, so the answer is complete and
// the stream ends where the controller expects it to.
static void TakeLateByte(Z2_STATE *pZ2, BYTE *pBuf, int len)
{
    BYTE tx = 0, last = 0;

    Z2Hw_Xfer(&tx, &last, 1, pZ2->frameHz, 0);
    memmove(pBuf, pBuf + 1, len - 1);
    pBuf[len - 1] = last;
    pZ2->realigns++;
}

static BOOL QueryFrameLen(Z2_STATE *pZ2, int *pLen)
{
    Z2_SPEED rd;
    BYTE tx[16], rx[16];
    int len;

    rd.hz = pZ2->frameHz;
    rd.txDelayUs = 0;
    rd.rxDelayUs = 0;
    memset(tx, 0, sizeof(tx));
    memset(rx, 0, sizeof(rx));
    tx[0] = pZ2->flipNop ? 0xeb : 0xea;
    tx[1] = pZ2->curNop;
    PutU16r(tx + 14, Sum(tx, 14));
    if (!Xfer(&rd, tx, rx, 16, TRUE))
    {
        SetErr(pZ2, Z2_ERR_QUERY_XFER, 0, tx, rx, 16);
        return FALSE;
    }
    if ((rx[0] & 0xf0) != 0xe0 && (rx[1] & 0xf0) == 0xe0)
        TakeLateByte(pZ2, rx, 16);      // reply one byte late: complete it
    if ((rx[0] & 0xf0) != 0xe0)
    {
        SetErr(pZ2, Z2_ERR_QUERY_TYPE, rx[0], tx, rx, 16);
        return FALSE;
    }
    if (GetLE16(rx + 14) != (Sum(rx, 14) & 0xffff))
    {
        SetErr(pZ2, Z2_ERR_QUERY_SUM, (int)(Sum(rx, 14) & 0xffff), tx, rx, 16);
        return FALSE;
    }
    len = rx[1] | (rx[2] << 8);
    if (len > pZ2->maxPacket || len + 5 > Z2_BUF_SIZE)
    {
        SetErr(pZ2, Z2_ERR_QUERY_LEN, len, tx, rx, 16);
        return FALSE;
    }
    *pLen = len;
    return TRUE;
}

static int ParseFrame(Z2_STATE *pZ2, const BYTE *pData, int len, Z2_FINGER *pFingers, int maxFingers)
{
    const BYTE *f, *end = pData + len;
    int headerLen, fingerLen, num, i, n = 0;

    if (len < FRAME_HEADER_LEN)
        return 0;
    headerLen = pData[2];
    num = pData[16];
    fingerLen = pData[17];
    if (headerLen < 12 || fingerLen < FINGER_LEN)
        return 0;

    f = pData + headerLen;
    for (i = 0; i < num && n < maxFingers; i++)
    {
        if (f + FINGER_LEN > end)
            break;
        pFingers[n].id = f[0];
        pFingers[n].event = f[1];
        pFingers[n].x = (SHORT)GetLE16(f + 4);
        pFingers[n].y = (SHORT)(pZ2->sensorHeight - (SHORT)GetLE16(f + 6));
        pFingers[n].sizeMajor = (WORD)GetLE16(f + 12);
        pFingers[n].sizeMinor = (WORD)GetLE16(f + 14);
        pFingers[n].forceMajor = (WORD)GetLE16(f + 18);
        pFingers[n].forceMinor = (WORD)GetLE16(f + 20);
        if (pFingers[n].forceMajor != 0 && pFingers[n].forceMinor != 0)
            n++;                        // fingers without pressure are not touching
        f += fingerLen;
    }
    return n;
}

int Z2_ReadFrame(Z2_STATE *pZ2, Z2_FINGER *pFingers, int maxFingers)
{
    int len, plen, n = 0;
    DWORD s;

    // after a bad frame, a preliminary length query resynchronises
    if (pZ2->flipNop && !pZ2->lastFrameOk)
    {
        if (!QueryFrameLen(pZ2, &len))
            goto bad;
    }
    if (!QueryFrameLen(pZ2, &len))
        goto bad;

    if (len == 0)
    {
        pZ2->empty++;
        goto ok;
    }

    len += 5;
    memset(g_ptx, 0, len);
    g_ptx[0] = pZ2->flipNop ? 0xeb : 0xea;
    g_ptx[1] = pZ2->curNop;
    g_ptx[2] = 1;
    s = Sum(g_ptx, 14);
    g_ptx[len - 2] = (BYTE)s;
    g_ptx[len - 1] = (BYTE)(s >> 8);
    memset(g_prx, 0, len);
    {
        Z2_SPEED rd;

        rd.hz = pZ2->frameHz;
        rd.txDelayUs = 0;
        rd.rxDelayUs = 0;
        if (!Xfer(&rd, g_ptx, g_prx, len, TRUE))
        {
            SetErr(pZ2, Z2_ERR_FRAME_XFER, len, g_ptx, g_prx, len);
            goto bad;
        }
    }

    if (g_prx[0] != 0xea && !(pZ2->flipNop && g_prx[0] == 0xeb) &&
        (g_prx[1] == 0xea || (pZ2->flipNop && g_prx[1] == 0xeb)))
        TakeLateByte(pZ2, g_prx, len);  // frame one byte late: complete it
    if (g_prx[0] != 0xea && !(pZ2->flipNop && g_prx[0] == 0xeb))
    {
        SetErr(pZ2, Z2_ERR_FRAME_TYPE, len, g_ptx, g_prx, len);
        goto bad;
    }
    if (Sum(g_prx, 5) & 0xff)
    {
        SetErr(pZ2, Z2_ERR_FRAME_HEADSUM, len, g_ptx, g_prx, len);
        goto bad;
    }
    plen = g_prx[2] | (g_prx[3] << 8);
    if (plen > 2)
    {
        if (5 + plen - 2 > len - 2)
        {
            SetErr(pZ2, Z2_ERR_FRAME_PLEN, plen, g_ptx, g_prx, len);
            goto bad;
        }
        s = Sum(g_prx + 5, plen - 2);
        if (GetLE16(g_prx + len - 2) != (s & 0xffff))
        {
            SetErr(pZ2, Z2_ERR_FRAME_SUM, (int)(s & 0xffff), g_ptx, g_prx, len);
            goto bad;
        }
        n = ParseFrame(pZ2, g_prx + 5, plen - 2, pFingers, maxFingers);
        pZ2->frames++;
    }

ok:
    pZ2->errStep = Z2_ERR_NONE;
    pZ2->lastFrameOk = TRUE;
    if (pZ2->flipNop)
        pZ2->curNop = (pZ2->curNop == 1) ? 2 : 1;
    return n;

bad:
    pZ2->lastFrameOk = FALSE;
    pZ2->errors++;
    return -1;
}
