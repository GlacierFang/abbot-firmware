/*
 * ui_boot_screen.c —— 开机画面
 *
 * 契约: abbot/doc/02 §3.1（ST7796S 竖屏 320×480, SPI 40MHz）
 *
 * 做法:
 *   - 图是编译进固件的 RGB565 静态数组 (src/ui/boot_image.c, 由
 *     tools/gen_boot_image.py 从 abbot/ui/poweron.png 生成), 不依赖 TF 卡与网络。
 *   - 数组内存字节序为大端 (MSB 先行), 与 ST7796S 的 16bit 收包顺序一致,
 *     也与 lv_conf.h 的 LV_COLOR_16_SWAP=1 下 LVGL color_t 的内存布局一致,
 *     所以图和 UI 走同一条 hal_display_blit_rgb565 通道, 颜色不会偏。
 *   - SPI 的 DMA 不能直接读 flash, 必须先把图搬进内部 SRAM 再逐块刷,
 *     所以按 20 行一块 memcpy + blit。
 *
 * 时机: main.cpp 在 hal_display_init() 之后、逐档点亮背光之前调用本函数。
 * 背光此时还是 0, 逐档点亮的过程正好把开机图"显影"出来; 之后 UI 首帧盖住它。
 */
#include "ui_boot_screen.h"

#include <Arduino.h>
#include <string.h>

#include "hal_shared.h"
#include "ui_boot_image.h"

/* 一次搬进 SRAM 的行数: 320 * 20 * 2 = 12800 字节, 内部 SRAM 放得下 */
#define UI_BOOT_BLOCK_LINES 20

/* 内部 SRAM 常驻块缓冲 (bss, 不占 flash) */
static uint16_t s_block[UI_BOOT_IMAGE_WIDTH * UI_BOOT_BLOCK_LINES];

void ui_boot_screen_show(void)
{
    for (int y = 0; y < UI_BOOT_IMAGE_HEIGHT; y += UI_BOOT_BLOCK_LINES) {
        memcpy(s_block, &ui_boot_image[(size_t)y * UI_BOOT_IMAGE_WIDTH],
               sizeof(uint16_t) * UI_BOOT_IMAGE_WIDTH * UI_BOOT_BLOCK_LINES);
        hal_display_blit_rgb565(0, y, UI_BOOT_IMAGE_WIDTH, UI_BOOT_BLOCK_LINES, s_block);
    }
}
