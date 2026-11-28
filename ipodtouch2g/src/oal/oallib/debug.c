//
// debug.c - OAL debug serial on UART0 (dock connector), ipodtouch2g BSP
//
// The dock UART is also the KITL transport. In KITL builds (SERIAL_KITL)
// raw debug output is suppressed so it does not corrupt the KITL stream;
// debug messages then travel through KITL to Platform Builder instead.
//
// The UART runs with iBoot's settings (s5l8720_uart.h).
//
#include <windows.h>
#include <oemglobal.h>
#include <nkintr.h>

#include <bsp.h>
#include <s5l8720_uart.h>

void Debug(LPCTSTR szFormat, ...);

//------------------------------------------------------------------------------
#ifndef SERIAL_KITL
// ---------------------------------------------------------------------------
// WriteDebugAscii: write a narrow string (for __DATE__ / __TIME__)
//
static void WriteDebugAscii(const char *psz)
{
    while (*psz != '\0')
        OEMWriteDebugByte((BYTE)*psz++);
}
#endif

// ---------------------------------------------------------------------------
// OEMInitDebugSerial: REQUIRED
//
// First OAL function the kernel calls (MMU on, before OEMInit). The I/O block
// is already mapped by OEMAddressTable, so the uncached VA is usable here.
//
void OEMInitDebugSerial(void)
{
#ifndef SERIAL_KITL
    S5lUart0Init();

    OEMWriteDebugString(L"\r\n\r\nipodtouch2g: OEMInitDebugSerial (UART0, iBoot's settings)\r\n");

    // build stamp, so a stale image is easy to spot
    OEMWriteDebugString(L"ipodtouch2g: built ");
    WriteDebugAscii(__DATE__);
    OEMWriteDebugByte(' ');
    WriteDebugAscii(__TIME__);
    OEMWriteDebugString(L"\r\n");
#endif
    return;
}

// ---------------------------------------------------------------------------
// OEMWriteDebugByte: REQUIRED
//
#ifndef SERIAL_KITL
static BYTE g_lastDebugByte = 0;
#endif

void OEMWriteDebugByte(BYTE bChar)
{
#ifdef SERIAL_KITL
    // KITL owns the UART
    UNREFERENCED_PARAMETER(bChar);
#else
    // Some components end their messages with LF only; add the CR so the
    // terminal goes back to column 0 (seen in the boot log 2026-11-28)
    if (bChar == '\n' && g_lastDebugByte != '\r')
        S5lUart0PutByte('\r');
    g_lastDebugByte = bChar;

    // the UART is reachable through the static I/O mapping at any time
    S5lUart0PutByte(bChar);
#endif
    return;
}

// ---------------------------------------------------------------------------
// OEMWriteDebugString: REQUIRED
//
void OEMWriteDebugString(LPWSTR pszStr)
{
    while (*pszStr != L'\0')
        OEMWriteDebugByte((BYTE)*pszStr++);
    return;
}

// ---------------------------------------------------------------------------
// OEMReadDebugByte: OPTIONAL
//
int OEMReadDebugByte(void)
{
#ifdef SERIAL_KITL
    return OEM_DEBUG_READ_NODATA;
#else
    int ch;

    ch = S5lUart0GetByte();
    return (ch < 0) ? OEM_DEBUG_READ_NODATA : ch;
#endif
}

// ---------------------------------------------------------------------------
// OEMWriteDebugLED: OPTIONAL (no LED on the iPod)
//
void OEMWriteDebugLED(WORD wIndex, DWORD dwPattern)
{
    UNREFERENCED_PARAMETER(wIndex);
    UNREFERENCED_PARAMETER(dwPattern);
    return;
}

// ---------------------------------------------------------------------------
// Debug: printf-style output through OEMWriteDebugString
//
void Debug(LPCTSTR szFormat, ...)
{
    TCHAR   szBuffer[256];
    va_list pArgs;

    va_start(pArgs, szFormat);
    NKwvsprintfW(szBuffer, szFormat, pArgs, sizeof(szBuffer) / sizeof(szBuffer[0]));
    va_end(pArgs);

    OEMWriteDebugString(szBuffer);
}
