//
// z2.h - Apple Zephyr2 multi-touch controller protocol (iPod touch 2G, N72)
//
// Written from the description in n72-linux's zephyr2.c (GPL, used as
// documentation) and openiBoot's multitouch-z2.c; see z2.c.
//
#ifndef __Z2_H
#define __Z2_H

#ifdef __cplusplus
extern "C" {
#endif

#define Z2_MAX_FINGERS      10

typedef struct
{
    BYTE    id;
    BYTE    event;
    SHORT   x;              // sensor units, 0 .. width
    SHORT   y;              // sensor units, 0 at the top (flipped as Linux does)
    WORD    forceMajor;
    WORD    forceMinor;
    WORD    sizeMajor;
    WORD    sizeMinor;
} Z2_FINGER;

typedef struct
{
    // filled by Z2_Boot
    DWORD   version;        // controller register 0x10008ffc
    int     familyId;
    int     rows, columns;
    int     bcdVersion;
    int     interfaceVersion;
    int     maxPacket;
    int     sensorWidth;    // sensor units (iOS: 5000 x 7500)
    int     sensorHeight;
    // frame reading state
    BOOL    flipNop;
    BYTE    curNop;
    BOOL    lastFrameOk;
    // frame read speed (registry FrameHz; Linux: 2 MHz)
    DWORD   frameHz;
    // statistics
    DWORD   frames, empty, errors;
    DWORD   realigns;       // answers that came one byte late and were completed
    // the last bad frame, for the log: which check failed and the bytes
    int     errStep;        // Z2_ERR_*
    int     errLen;         // length of the transfer in errTx/errRx
    int     errValue;       // the value the check looked at
    BYTE    errTx[16];
    BYTE    errRx[16];
} Z2_STATE;

// Z2_STATE.errStep: where a frame read failed
#define Z2_ERR_NONE             0
#define Z2_ERR_QUERY_XFER       1   // length query: SPI timeout
#define Z2_ERR_QUERY_TYPE       2   // length query: answer byte 0 not 0xeN
#define Z2_ERR_QUERY_SUM        3   // length query: checksum (bytes 14-15) wrong
#define Z2_ERR_QUERY_LEN        4   // length query: length > max packet
#define Z2_ERR_FRAME_XFER       5   // frame read: SPI timeout
#define Z2_ERR_FRAME_TYPE       6   // frame: byte 0 not 0xea/0xeb
#define Z2_ERR_FRAME_HEADSUM    7   // frame: sum of bytes 0-4 not 0 (mod 256)
#define Z2_ERR_FRAME_PLEN       8   // frame: payload length too long for the read
#define Z2_ERR_FRAME_SUM        9   // frame: payload checksum wrong

// errStep as text, for the log
const WCHAR *Z2_ErrName(int step);

// Start the controller: reset, firmware, calibration, calibrate, execute,
// interface version, info reports. Logs every step. TRUE on success.
// frameHz: frame read speed (0 = 2 MHz as Linux; the driver passes registry FrameHz).
BOOL Z2_Boot(Z2_STATE *pZ2, const BYTE *pFw, DWORD fwLen, const BYTE *pCal, DWORD calLen,
             DWORD frameHz);

// Read one frame (call when data ready is low). Returns the number of
// fingers in it (0 = none or empty frame), or -1 on an error.
int  Z2_ReadFrame(Z2_STATE *pZ2, Z2_FINGER *pFingers, int maxFingers);

#ifdef __cplusplus
}
#endif

#endif // __Z2_H
