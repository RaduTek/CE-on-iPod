/*
 * whlcore.c - what the iPod's click wheel controller says, taken apart
 * (see whlcore.h)
 */
#include "whlcore.h"

void Whl_Reset(WHL_STATS *s)
{
    unsigned char *p = (unsigned char *)s;
    unsigned int i;

    for (i = 0; i < sizeof(*s); i++)
        p[i] = 0;
}

void Whl_Account(WHL_STATS *s, unsigned int flags, unsigned int rx)
{
    unsigned int i, pos;

    s->samples++;
    s->flagsNow = flags;
    if (flags != 0)
    {
        s->flagSamples++;
        s->flagsSeen |= flags;
    }
    if (s->samples > 1 && rx == s->rxNow)
        return;

    /* a new report (the very first look counts as one) */
    s->rxNow = rx;
    s->changes++;
    for (i = WHL_LAST - 1; i > 0; i--)
        s->last[i] = s->last[i - 1];
    s->last[0] = rx;

    if ((rx & WHL_PKT_MASK) == WHL_PKT_WHEEL)
    {
        s->packets++;
        s->buttonsNow = (rx >> 8) & 0x1F;
        s->buttonsSeen |= s->buttonsNow;
        s->touchedNow = (rx & WHL_TOUCHED) ? 1 : 0;
        if (s->touchedNow)
        {
            pos = (rx >> 16) & 0x7F;
            s->posNow = pos;
            if (s->touches == 0 || pos < s->posMin)
                s->posMin = pos;
            if (s->touches == 0 || pos > s->posMax)
                s->posMax = pos;
            s->touches++;
        }
    }
    else if ((rx & WHL_PKT_INITMASK) == WHL_PKT_INIT)
    {
        s->startPackets++;
        s->buttonsNow = (rx >> 16) & 0x1F;
        s->buttonsSeen |= s->buttonsNow;
    }
    else
        s->others++;
}

int Whl_Delta(unsigned int from, unsigned int to)
{
    int d = (int)(to % WHL_POSITIONS) - (int)(from % WHL_POSITIONS);

    if (d < -(WHL_POSITIONS / 2 - 1))
        d += WHL_POSITIONS;
    else if (d > WHL_POSITIONS / 2)
        d -= WHL_POSITIONS;
    return d;
}

static int Push(WHL_EVENT *out, int n, int kind, int a, int b, unsigned int ms)
{
    if (n < WHL_MAX_EVENTS)
    {
        out[n].kind = (unsigned char)kind;
        out[n].a = (unsigned char)a;
        out[n].b = (unsigned char)b;
        out[n].pad = 0;
        out[n].ms = ms;
        n++;
    }
    return n;
}

int Whl_Feed(WHL_INPUT *in, unsigned int rx, unsigned int nowMs, WHL_EVENT *out)
{
    unsigned int buttons, changed, bit, pos;
    int n = 0;

    if ((rx & WHL_PKT_MASK) != WHL_PKT_WHEEL)
        return 0;                           /* a start-up answer, or nothing this driver knows */

    buttons = (rx >> 8) & 0x1F;
    if (!in->started)
    {
        /* what is held at the first report was pressed before this began (the
           button that started Windows CE, say): no press to pass on */
        in->buttons = buttons;
        in->started = 1;
    }
    changed = buttons ^ in->buttons;
    for (bit = 1; bit <= 0x10; bit <<= 1)
    {
        if (changed & bit)
            n = Push(out, n, WHL_EV_BUTTON, (int)bit, (buttons & bit) ? 1 : 0, nowMs);
    }
    in->buttons = buttons;

    if (!(rx & WHL_TOUCHED))
    {
        in->touched = 0;                    /* the finger is off (for the moment, perhaps) */
        return n;
    }
    pos = (rx >> 16) & 0x7F;
    if (pos >= WHL_POSITIONS)
        return n;
    if (!in->touched && (!in->everTouched || nowMs - in->touchMs > WHL_UNTOUCH_MS))
    {
        /* a new touch: this is where it starts, nothing has moved yet */
        in->accum = 0;
        in->pos = (int)pos;
    }
    else
    {
        in->accum += Whl_Delta((unsigned int)in->pos, pos);
        in->pos = (int)pos;
        while (in->accum >= WHL_STEP_POSITIONS)
        {
            n = Push(out, n, WHL_EV_STEP, 1, 0, nowMs);
            in->accum -= WHL_STEP_POSITIONS;
        }
        while (in->accum <= -WHL_STEP_POSITIONS)
        {
            n = Push(out, n, WHL_EV_STEP, 0, 0, nowMs);
            in->accum += WHL_STEP_POSITIONS;
        }
    }
    in->touched = 1;
    in->everTouched = 1;
    in->touchMs = nowMs;
    return n;
}
