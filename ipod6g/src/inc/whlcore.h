/*
 * whlcore.h - what the iPod's click wheel controller says, taken apart
 *
 * The controller hands over one 32-bit word per report (register WHEELRX).
 * Its layout is the one Rockbox reads (firmware/target/arm/ipod/
 * button-clickwheel.c):
 *
 *   (word & 0x800000FF) == 0x8000001A   buttons and wheel
 *        bit 8 centre, 9 next, 10 previous, 11 play, 12 menu (1 = held)
 *        bit 30 = a finger is on the wheel, bits 16..22 = where (0 .. 95,
 *        clockwise counts up)
 *   (word & 0x8000FFFF) == 0x8000023A   the answer to the start-up command,
 *        with the buttons in bits 16..20
 *
 * Plain C with no Windows in it, so the PC can test it.
 */
#ifndef WHLCORE_H
#define WHLCORE_H

#ifdef __cplusplus
extern "C" {
#endif

#define WHL_PKT_MASK        0x800000FFu
#define WHL_PKT_WHEEL       0x8000001Au
#define WHL_PKT_INITMASK    0x8000FFFFu
#define WHL_PKT_INIT        0x8000023Au
#define WHL_TOUCHED         0x40000000u
#define WHL_POSITIONS       96

#define WHL_BTN_CENTRE      0x01
#define WHL_BTN_NEXT        0x02
#define WHL_BTN_PREVIOUS    0x04
#define WHL_BTN_PLAY        0x08
#define WHL_BTN_MENU        0x10

#define WHL_LAST            6

/* what a stretch of looking at the controller added up to */
typedef struct {
    unsigned int samples;       /* looks at the two registers */
    unsigned int flagSamples;   /* looks at which the flags register was not 0 */
    unsigned int flagsSeen;     /* every flag bit seen */
    unsigned int flagsNow;
    unsigned int changes;       /* times the report read different from the look before */
    unsigned int packets;       /* changes to a buttons-and-wheel report */
    unsigned int startPackets;  /* changes to a start-up answer */
    unsigned int others;        /* changes to anything else */
    unsigned int buttonsSeen;   /* WHL_BTN_ bits, every button seen held */
    unsigned int buttonsNow;
    unsigned int touches;       /* buttons-and-wheel reports with a finger on the wheel */
    unsigned int touchedNow;
    unsigned int posNow;
    unsigned int posMin, posMax;    /* valid when touches != 0 */
    unsigned int cleared;       /* times flags were written back (0 while only looking) */
    unsigned int rxNow;
    unsigned int last[WHL_LAST];    /* the last different reports, newest first */
} WHL_STATS;

void Whl_Reset(WHL_STATS *s);

/* One look: the flags register and the report register as just read. */
void Whl_Account(WHL_STATS *s, unsigned int flags, unsigned int rx);

/* How far the wheel moved between two positions, the short way round:
   -47 .. +48, clockwise positive. */
int  Whl_Delta(unsigned int from, unsigned int to);

/*
 * Reports turned into events, for a driver.
 */
#define WHL_EV_BUTTON       1   /* a = which (one WHL_BTN_ bit), b = 1 pressed, 0 let go */
#define WHL_EV_STEP         2   /* a = 1 one step clockwise, 0 one step the other way */

#define WHL_STEP_POSITIONS  4   /* wheel positions to a step: 24 steps a turn, as Rockbox has it */
#define WHL_UNTOUCH_MS      150 /* a finger off the wheel for less than this never left */
#define WHL_MAX_EVENTS      16  /* at most this many from one report */

typedef struct {
    unsigned char kind, a, b, pad;
    unsigned int ms;            /* when (the clock the driver feeds in) */
} WHL_EVENT;

typedef struct {
    unsigned int buttons;       /* held, as last reported */
    int     touched;            /* a finger is on the wheel */
    int     pos;                /* where it was last */
    int     accum;              /* positions moved and not yet turned into a step */
    unsigned int touchMs;       /* when it was last seen on the wheel */
    int     everTouched;
    int     started;            /* a first report was seen */
} WHL_INPUT;

/* One report (call it when the report register reads different from before).
   Writes the events it means to out[WHL_MAX_EVENTS], returns how many. */
int  Whl_Feed(WHL_INPUT *in, unsigned int rx, unsigned int nowMs, WHL_EVENT *out);

#ifdef __cplusplus
}
#endif

#endif
