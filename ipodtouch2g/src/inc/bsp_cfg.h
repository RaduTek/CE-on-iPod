//
// bsp_cfg.h - static SYSINTR assignments for the ipodtouch2g BSP
//
// The OAL uses a fixed SYSINTR <-> IRQ table (intr.c), as the ipod6g and
// CEonPi BSPs do. So far only the kernel's SYSINTR_RESCHED is used
// (timer 4 on S5L_IRQ_TIMER). KITL polls the UART, so the UART needs no
// SYSINTR (its IRQ number is not known yet either).
//
#ifndef __BSP_CFG_H
#define __BSP_CFG_H

#if __cplusplus
extern "C" {
#endif

// Driver SYSINTRs start at SYSINTR_FIRMWARE+0 when the first driver needs one.

#if __cplusplus
}
#endif

#endif
