//
// lcdhw.cpp - S5L8702 LCD interface + iPod classic panel access (CPU writes)
//
// First version: every command and pixel is written by the CPU, polling the
// interface status. DMA (PL080 DMAC0, peripheral 3 = LCD_WR) comes later.
//
#include <windows.h>
#include <ceddk.h>

#include "lcdhw.h"

//------------------------------------------------------------------------------
// S5L8702 LCD interface registers (Rockbox s5l87xx.h, LCD_BASE for S5L8702)
//
#define LCD_PA_BASE         0x38300000
#define LCD_CON             0x00        // control / interface mode
#define LCD_WCMD            0x04        // write command
#define LCD_STATUS          0x1C        // interface status
#define LCD_PHTIME          0x20        // phase time
#define LCD_WDATA           0x40        // write data

#define LCD_STATUS_READY    0x02        // may write LCD_CON
#define LCD_STATUS_BUSY     0x10        // must not write WCMD/WDATA

// LCD_CON values (Rockbox lcd-s5l8702.c)
#define LCD_MODE_P8         0x80000C20  // 8-bit bus: 8-bit command set panels
#define LCD_MODE_P18        0x80000DA8  // 18-bit bus: 16-bit command set panels
#define LCD_MODE_P16        0x80100DB0  // frame mode: RGB565, one transfer per pixel

// Panel commands, 8-bit command set (panel types 0 and 1, ILI9340-like)
#define R_COLUMN_ADDR_SET       0x2A
#define R_ROW_ADDR_SET          0x2B
#define R_MEMORY_WRITE          0x2C

// Panel registers, 16-bit command set (panel types 2 and 3, ILI9320/9326-like)
#define R_HORIZ_GRAM_ADDR_SET   0x200
#define R_VERT_GRAM_ADDR_SET    0x201
#define R_WRITE_DATA_TO_GRAM    0x202
#define R_HORIZ_ADDR_START_POS  0x210
#define R_HORIZ_ADDR_END_POS    0x211
#define R_VERT_ADDR_START_POS   0x212
#define R_VERT_ADDR_END_POS     0x213

// GPIO: panel type in PDAT6 bits 5:4
#define GPIO_PA_BASE        0x3CF00000
#define GPIO_PDAT6          (6 * 0x20 + 0x04)

// Clock gates: LCD = PWRCON0 (AHB) bit 1
#define CLK_PA_BASE         0x3C500000
#define CLK_PWRCON0         0x48
#define PWRCON0_LCD         (1 << 1)

// Busy-wait limit: a few ms; the interface normally answers in < 1 us
#define LCD_WAIT_LOOPS      200000

//------------------------------------------------------------------------------
// State
//
static volatile UINT32 *g_pLcd = NULL;
static int g_panelType = -1;
static BOOL g_cmdset16 = FALSE;
static DWORD g_cmdMode = LCD_MODE_P8;
static BOOL g_timeoutReported = FALSE;

#define LCD_REG(off)    g_pLcd[(off) >> 2]

//------------------------------------------------------------------------------
// Low level (Rockbox s5l_lcd_write_config / write_cmd / write_data)
//
static BOOL WaitStatus(UINT32 mask, BOOL wantSet)
{
    int i;

    for (i = 0; i < LCD_WAIT_LOOPS; i++)
    {
        UINT32 status = LCD_REG(LCD_STATUS);
        if (((status & mask) != 0) == (wantSet != FALSE))
            return TRUE;
    }

    if (!g_timeoutReported)
    {
        g_timeoutReported = TRUE;
        RETAILMSG(1, (L"ipodlcd: LCD interface timeout (status 0x%08x, mask 0x%x)\r\n",
                      LCD_REG(LCD_STATUS), mask));
    }
    return FALSE;
}

static BOOL WriteConfig(UINT32 config)
{
    int i;

    if (!WaitStatus(LCD_STATUS_READY, TRUE))
        return FALSE;

    // Rockbox waits 1 us here; a few uncached register reads cover that
    for (i = 0; i < 64; i++)
        (void)LCD_REG(LCD_STATUS);

    LCD_REG(LCD_CON) = config;
    return TRUE;
}

static __inline BOOL WriteCmd(UINT32 cmd)
{
    if (!WaitStatus(LCD_STATUS_BUSY, FALSE))
        return FALSE;
    LCD_REG(LCD_WCMD) = cmd;
    return TRUE;
}

static __inline BOOL WriteData(UINT32 data)
{
    if (!WaitStatus(LCD_STATUS_BUSY, FALSE))
        return FALSE;
    LCD_REG(LCD_WDATA) = data;
    return TRUE;
}

static BOOL WriteReg16(UINT32 reg, UINT32 value)
{
    return WriteCmd(reg) && WriteData(value);
}

static BOOL SendCmd8(UINT32 cmd, const UINT8 *pData, int len)
{
    if (!WriteCmd(cmd))
        return FALSE;
    while (len-- > 0)
    {
        if (!WriteData(*pData++))
            return FALSE;
    }
    return TRUE;
}

// Set the GRAM window and start a memory write (Rockbox displaylcd_setup)
static BOOL SetWindow(int x, int y, int w, int h)
{
    int xe = x + w - 1;
    int ye = y + h - 1;

    if (!WriteConfig(g_cmdMode))
        return FALSE;

    if (g_cmdset16)
    {
        return WriteReg16(R_HORIZ_ADDR_START_POS, x) &&
               WriteReg16(R_HORIZ_ADDR_END_POS, xe) &&
               WriteReg16(R_VERT_ADDR_START_POS, y) &&
               WriteReg16(R_VERT_ADDR_END_POS, ye) &&
               WriteReg16(R_HORIZ_GRAM_ADDR_SET, x) &&
               WriteReg16(R_VERT_GRAM_ADDR_SET, y) &&
               WriteCmd(R_WRITE_DATA_TO_GRAM);
    }
    else
    {
        UINT8 col[4], row[4];

        col[0] = (UINT8)(x >> 8);  col[1] = (UINT8)x;
        col[2] = (UINT8)(xe >> 8); col[3] = (UINT8)xe;
        row[0] = (UINT8)(y >> 8);  row[1] = (UINT8)y;
        row[2] = (UINT8)(ye >> 8); row[3] = (UINT8)ye;

        return SendCmd8(R_COLUMN_ADDR_SET, col, 4) &&
               SendCmd8(R_ROW_ADDR_SET, row, 4) &&
               WriteCmd(R_MEMORY_WRITE);
    }
}

//------------------------------------------------------------------------------
// Public
//
BOOL LcdHwInit(void)
{
    PHYSICAL_ADDRESS pa;
    volatile UINT32 *pGpio;
    volatile UINT32 *pClk;
    UINT32 pdat6;

    pa.QuadPart = LCD_PA_BASE;
    g_pLcd = (volatile UINT32 *)MmMapIoSpace(pa, 0x1000, FALSE);

    pa.QuadPart = GPIO_PA_BASE;
    pGpio = (volatile UINT32 *)MmMapIoSpace(pa, 0x1000, FALSE);

    pa.QuadPart = CLK_PA_BASE;
    pClk = (volatile UINT32 *)MmMapIoSpace(pa, 0x1000, FALSE);

    if (g_pLcd == NULL || pGpio == NULL || pClk == NULL)
    {
        RETAILMSG(1, (L"ipodlcd: MmMapIoSpace failed (lcd %08x gpio %08x clk %08x)\r\n",
                      g_pLcd, pGpio, pClk));
        return FALSE;
    }

    // Make sure the LCD interface is clocked (the bootloader leaves it on)
    if (pClk[CLK_PWRCON0 >> 2] & PWRCON0_LCD)
    {
        RETAILMSG(1, (L"ipodlcd: LCD clock was gated, enabling\r\n"));
        pClk[CLK_PWRCON0 >> 2] &= ~PWRCON0_LCD;
    }

    pdat6 = pGpio[GPIO_PDAT6 >> 2];
    g_panelType = (int)((pdat6 >> 4) & 0x3);

    // types 0/1: 8-bit command set; types 2/3: 16-bit command set
    g_cmdset16 = (g_panelType >= 2);
    g_cmdMode = g_cmdset16 ? LCD_MODE_P18 : LCD_MODE_P8;

    RETAILMSG(1, (L"ipodlcd: panel type %d (%s command set), LCD_CON 0x%08x, STATUS 0x%08x, PHTIME 0x%08x\r\n",
                  g_panelType, g_cmdset16 ? L"16-bit" : L"8-bit",
                  LCD_REG(LCD_CON), LCD_REG(LCD_STATUS), LCD_REG(LCD_PHTIME)));

    MmUnmapIoSpace((PVOID)pGpio, 0x1000);
    MmUnmapIoSpace((PVOID)pClk, 0x1000);

    return WriteConfig(g_cmdMode);
}

int LcdHwPanelType(void)
{
    return g_panelType;
}

BOOL LcdHwPushRect(const BYTE *pFrame, int stride, int x, int y, int w, int h)
{
    int row, col;

    if (g_pLcd == NULL || w <= 0 || h <= 0)
        return FALSE;

    if (!SetWindow(x, y, w, h))
        return FALSE;

    // frame mode: one RGB565 pixel per write
    if (!WriteConfig(LCD_MODE_P16))
        return FALSE;

    for (row = 0; row < h; row++)
    {
        const UINT16 *pLine = (const UINT16 *)(pFrame + (y + row) * stride) + x;

        for (col = 0; col < w; col++)
        {
            if (!WaitStatus(LCD_STATUS_BUSY, FALSE))
                return FALSE;
            LCD_REG(LCD_WDATA) = pLine[col];
        }
    }

    // back to command mode once the data has drained
    return WriteConfig(g_cmdMode);
}
