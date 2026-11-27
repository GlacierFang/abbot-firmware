/*
 * ui_wifi_bg.h —— 首屏（PAGE_OOBE）背景位图
 *
 * 由 tools/gen_wifi_bg.py 从 abbot/ui/wificonfig.png 生成 (src/ui/wifi_bg.c)。
 *
 * 数组是 RGB565、**LVGL 原生 16 位色内存布局**:
 * lv_conf.h 里 LV_COLOR_DEPTH=16 且 LV_COLOR_16_SWAP=1, LVGL 的 lv_color_t
 * 位布局是 green_h|red<<3|blue<<8|green_l<<13 (见 LVGL v8 lv_color.h),
 * 所以本数组可以直接作为 lv_img_dsc_t (LV_IMG_CF_TRUE_COLOR) 的数据源,
 * 颜色与 UI 其它元素一致。
 *
 * 注意: 这个数值约定与开机图 (src/ui/boot_image.c) **完全相同** ——
 * 两边都是「规范 RGB565 的两个字节按 MSB 在前存放」。
 * 区别只在用法: 开机图直接喂 hal_display_blit_rgb565 送屏;
 * 本数组给 LVGL 的 lv_img_dsc_t (LV_IMG_CF_TRUE_COLOR) 用,
 * 因此数组内容必须与 LVGL 的 lv_color_t 内存布局逐位一致。
 *
 * 前提: lv_conf.h 里 LV_IMG_CACHE_DEF_SIZE 必须是 0。
 * 图缓存会把整张图 (320x480x2 = 300KB) 搬进 LVGL 的堆 (LV_MEM_SIZE 只有 48KB) ——
 * 一旦开启缓存, 这张图就分配不出来, 首屏会整屏空白。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_WIFI_BG_WIDTH 320
#define UI_WIFI_BG_HEIGHT 480
#define UI_WIFI_BG_PIXELS (UI_WIFI_BG_WIDTH * UI_WIFI_BG_HEIGHT)

extern const uint16_t ui_wifi_bg[UI_WIFI_BG_PIXELS];

/* 探针: 固定位置的规范 RGB565 数值, 供主机单测确定性校验内存位布局 */
#define UI_WIFI_BG_PROBE_COUNT 6
extern const uint32_t ui_wifi_bg_probe_idx[UI_WIFI_BG_PROBE_COUNT];
extern const uint16_t ui_wifi_bg_probe_val[UI_WIFI_BG_PROBE_COUNT];

#ifdef __cplusplus
}
#endif
