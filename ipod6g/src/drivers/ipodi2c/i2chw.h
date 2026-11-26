//
// i2chw.h - the S5L8702's I2C controller 0, polled (i2chw.c), for ipodi2c.c
//
#ifndef I2CHW_H
#define I2CHW_H

#ifdef __cplusplus
extern "C" {
#endif

// where a failed transfer stopped (IPODI2C_STEP_..., IPODI2C_E_...)
typedef struct {
    DWORD   step;
    DWORD   code;
    DWORD   stat;           // IICSTAT, read while the controller's clock was still on
    DWORD   sta2;           // IICSTA2
} I2CHW_FAIL;

// Maps the registers. Writes nothing. Returns FALSE when they cannot be mapped.
BOOL  I2cHw_Map(DWORD *pBootPcon3, DWORD *pBootPwrcon1);

// One transfer each; chip is the 7-bit address. Not reentrant: the caller
// serialises. Every path ends with the bus released and the controller's
// clock off. Returns IPODI2C_OK or an IPODI2C_E_ code (details in *pFail).
DWORD I2cHw_Read(BYTE chip, BYTE reg, BYTE *pOut, DWORD count, I2CHW_FAIL *pFail);
DWORD I2cHw_Write(BYTE chip, BYTE reg, const BYTE *pData, DWORD count, I2CHW_FAIL *pFail);

#ifdef __cplusplus
}
#endif

#endif
