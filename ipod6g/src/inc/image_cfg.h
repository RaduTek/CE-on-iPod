//
// image_cfg.h - image and memory configuration for the ipod6g BSP
//
// Keep in sync with files\config.bib and OEMAddrTable.inc.
//
#ifndef __IMAGE_CFG_H
#define __IMAGE_CFG_H

#if __cplusplus
extern "C" {
#endif

#define BSP_SPEED_CPUMHZ                    216

//------------------------------------------------------------------------------
// Platform / processor strings (IOCTL_HAL_GET_DEVICE_INFO)
//
#define IOCTL_PLATFORM_TYPE                 (L"iPod classic")
#define IOCTL_PLATFORM_OEM                  (L"CE-on-iPod")
#define IOCTL_PLATFORM_MANUFACTURER         (L"Apple")
#define IOCTL_PLATFORM_NAME                 (L"ipod6g")

#define IOCTL_PROCESSOR_VENDOR              (L"Samsung")
#define IOCTL_PROCESSOR_NAME                (L"S5L8702")
#define IOCTL_PROCESSOR_CORE                (L"ARM926EJ-S")

//------------------------------------------------------------------------------
// Memory map
//
//   PA           VA (cached)    Size
//   0800 0000    8000 0000      0400 0000     SDRAM (64 MB)
//   3800 0000    8400 0000      0800 0000     S5L8702 I/O
//
// The image (nk.nb0) is loaded by the Rockbox bootloader at PA 0x08000000,
// i.e. VA 0x80000000, the start of the NK region.
//
#define DRAM_BASE_PA_START                  0x08000000
#define DRAM_BASE_CA_START                  0x80000000
#define DRAM_SIZE                           0x04000000

// Scheduler timer constants
#define RESCHED_PERIOD                      1           // ms per system tick
#define OEM_COUNT_1MS                       1000        // perf counter (Timer E) counts per ms

//------------------------------------------------------------------------------

#if __cplusplus
}
#endif

#endif
