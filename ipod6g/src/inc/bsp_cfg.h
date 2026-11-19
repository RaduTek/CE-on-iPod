//
// bsp_cfg.h - static SYSINTR assignments for the ipod6g BSP
//
// The OAL uses a fixed SYSINTR <-> IRQ table (intr.c) instead of the
// PQOAL dynamic mapping, as in the CEonPi BSP.
//
#ifndef __BSP_CFG_H
#define __BSP_CFG_H

#if __cplusplus
extern "C" {
#endif

// SYSINTR_RESCHED (kernel defined) <-> S5L_IRQ_TIMER (timer B tick)
#define SYSINTR_UART0           (SYSINTR_FIRMWARE+0)    // S5L_IRQ_UART0

#define SYSINTR_ENDOFSTATIC     (SYSINTR_FIRMWARE+0)

#if __cplusplus
}
#endif

#endif
