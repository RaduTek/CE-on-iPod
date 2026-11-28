//
// image_cfg.h - image and memory configuration for the ipodtouch2g BSP
//
// Keep in sync with files\config.bib and OEMAddrTable.inc.
//
#ifndef __IMAGE_CFG_H
#define __IMAGE_CFG_H

#if __cplusplus
extern "C" {
#endif

#define BSP_SPEED_CPUMHZ                    533     // nominal; not measured

//------------------------------------------------------------------------------
// Platform / processor strings (IOCTL_HAL_GET_DEVICE_INFO)
//
#define IOCTL_PLATFORM_TYPE                 (L"iPod touch 2G")
#define IOCTL_PLATFORM_OEM                  (L"CE-on-iPod")
#define IOCTL_PLATFORM_MANUFACTURER         (L"Apple")
#define IOCTL_PLATFORM_NAME                 (L"ipodtouch2g")

#define IOCTL_PROCESSOR_VENDOR              (L"Samsung")
#define IOCTL_PROCESSOR_NAME                (L"S5L8720")
#define IOCTL_PROCESSOR_CORE                (L"ARM1176JZF-S")

//------------------------------------------------------------------------------
// Memory map
//
//   PA           VA (cached)    Size
//   0800 0000    8000 0000      0800 0000     DRAM (128 MB)
//   3800 0000    8800 0000      0800 0000     S5L8720 I/O
//
// CE's RAM ends at PA 0x0FC00000 (VA 0x87C00000); the top 4 MB hold the
// framebuffer (and probably iBoot). The image runs at PA 0x08000000,
// i.e. VA 0x80000000, the start of the NK region; startup.s moves it there
// if iBoot started it elsewhere.
//
#define DRAM_BASE_PA_START                  0x08000000
#define DRAM_BASE_CA_START                  0x80000000
#define DRAM_SIZE                           0x08000000

// Scheduler timer
#define RESCHED_PERIOD                      1           // ms per system tick

//------------------------------------------------------------------------------

#if __cplusplus
}
#endif

#endif
