//
// bsp.h - standard include hierarchy for the ipod6g BSP
//
#ifndef __BSP_H
#define __BSP_H

#if __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <ceddk.h>
#include <nkintr.h>
#include <oal.h>

// BSP configuration
#include "image_cfg.h"
#include "bsp_cfg.h"

// SoC definitions
#include "s5l8702.h"

#if __cplusplus
}
#endif

#endif
