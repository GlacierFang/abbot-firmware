/*
 * ui_boot_image.h —— 开机画面位图
 *
 * 由 tools/gen_boot_image.py 从 abbot/ui/poweron.png 生成 (src/ui/boot_image.c)。
 * 数组是 RGB565、大端字节序 (MSB 先行), 与 lv_conf.h 的 LV_COLOR_16_SWAP=1
 * 以及 ST7796S 的 SPI 收包顺序一致, 可直接喂给 hal_display_blit_rgb565。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_BOOT_IMAGE_WIDTH 320
#define UI_BOOT_IMAGE_HEIGHT 480
#define UI_BOOT_IMAGE_PIXELS (UI_BOOT_IMAGE_WIDTH * UI_BOOT_IMAGE_HEIGHT)

extern const uint16_t ui_boot_image[UI_BOOT_IMAGE_PIXELS];

/* 探针: 固定位置的规范 RGB565 数值, 供主机单测确定性校验内存字节序 */
#define UI_BOOT_IMAGE_PROBE_COUNT 6
extern const uint32_t ui_boot_image_probe_idx[UI_BOOT_IMAGE_PROBE_COUNT];
extern const uint16_t ui_boot_image_probe_val[UI_BOOT_IMAGE_PROBE_COUNT];

#ifdef __cplusplus
}
#endif
