//
// lcdhw.h - S5L8702 LCD interface + iPod classic panel access
//
// The iPod classic panel is a command-based (8080 MPU bus) controller with
// its own GRAM, not a scanned framebuffer: pixels are sent to a window in
// panel GRAM through the S5L8702 LCD interface (LCD_WDATA). The Rockbox
// bootloader has already run the panel init sequence, so only the window
// setup and data transfer are needed here.
//
// Reference: Rockbox firmware/target/arm/s5l8702/lcd-s5l8702.c and
// ipod6g/lcd-6g.c (GPL; used as documentation only).
//
#ifndef __LCDHW_H
#define __LCDHW_H

#include <windows.h>

#define LCD_WIDTH       320
#define LCD_HEIGHT      240

#ifdef __cplusplus
extern "C" {
#endif

// Map the registers, read the panel type and select command mode.
// Returns FALSE if the registers cannot be mapped.
BOOL LcdHwInit(void);

// Panel type 0-3 read from PDAT6[5:4] (valid after LcdHwInit)
int LcdHwPanelType(void);

// Send the rectangle (x, y, w, h) of an RGB565 framebuffer with the given
// stride (in bytes) to the panel. Blocks until the data has been written.
// Returns FALSE if the LCD interface timed out.
BOOL LcdHwPushRect(const BYTE *pFrame, int stride, int x, int y, int w, int h);

#ifdef __cplusplus
}
#endif

#endif // __LCDHW_H
