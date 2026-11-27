/*
 * test_wifi_bg.c —— 首屏背景位图的静态自检（主机侧）
 *
 * 检的是「这张图有没有被做坏」，不是「像素长什么样」：
 *   1. 尺寸常量与屏幕一致 (320x480), 数组长度对得上。
 *   2. 内存位布局与 LVGL 的 lv_color_t 一致: 用生成器写死的探针位置做**确定性**校验 ——
 *      数组里那个 uint16 换算回来应当正好等于该像素的规范 RGB565。
 *      位布局写反了这条立刻炸, 不依赖任何启发式。
 *   3. 内容非退化: 颜色种类够多, 既有暗部也有亮部 (纯色/全黑/全白都要被挡住)。
 */
#include <stdio.h>
#include <string.h>

#include "ui_wifi_bg.h"

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *msg)
{
    g_checks++;
    if (!cond) {
        g_fail++;
        printf("FAIL: %s\n", msg);
    }
}

/* 数组里取出的 uint16 -> 规范 RGB565。
 * LV_COLOR_16_SWAP=1 时 lv_color_t 的位布局是
 *   bit 0-2 green_h | bit 3-7 red | bit 8-12 blue | bit 13-15 green_l
 * 换算回来: blue | green<<5 | red<<11。 */
static unsigned canonical_rgb565(uint16_t stored)
{
    unsigned green_h = stored & 0x7U;
    unsigned red = (stored >> 3) & 0x1FU;
    unsigned blue = (stored >> 8) & 0x1FU;
    unsigned green_l = (stored >> 13) & 0x7U;
    unsigned green = (green_h << 3) | green_l;
    return blue | (green << 5) | (red << 11);
}

int main(void)
{
    check(UI_WIFI_BG_WIDTH == 320, "宽度必须是 320");
    check(UI_WIFI_BG_HEIGHT == 480, "高度必须是 480");
    check(UI_WIFI_BG_PIXELS == 320 * 480, "像素总数必须是 320*480");

    /* 位布局探针: 生成器把固定位置的规范 RGB565 写死在探针数组里 */
    for (int i = 0; i < UI_WIFI_BG_PROBE_COUNT; i++) {
        uint32_t idx = ui_wifi_bg_probe_idx[i];
        check(idx < (uint32_t)UI_WIFI_BG_PIXELS, "探针下标越界");
        unsigned got = canonical_rgb565(ui_wifi_bg[idx]);
        unsigned want = ui_wifi_bg_probe_val[i];
        if (got != want) {
            printf("FAIL: 探针 %d (idx=%u) 位布局不符: got 0x%04X want 0x%04X\n", i, idx, got, want);
            g_fail++;
            g_checks++;
        } else {
            g_checks++;
        }
    }

    /* 内容非退化: 颜色种类够多, 且既有暗部也有亮部 */
    unsigned long kinds = 0;
    unsigned min_lum = 0xFFFF;
    unsigned max_lum = 0;
    static unsigned short seen[8192];
    memset(seen, 0, sizeof(seen));
    for (uint32_t i = 0; i < UI_WIFI_BG_PIXELS; i++) {
        unsigned v = canonical_rgb565(ui_wifi_bg[i]);
        unsigned short key = (unsigned short)(v >> 3); /* 5-6-5 折成 5-5-5 近似 */
        if (!seen[key]) {
            seen[key] = 1;
            kinds++;
        }
        unsigned lum = (((v >> 11) & 0x1FU) * 2 + ((v >> 5) & 0x3FU) + ((v & 0x1FU) / 2));
        if (lum < min_lum) {
            min_lum = lum;
        }
        if (lum > max_lum) {
            max_lum = lum;
        }
    }
    printf("kinds=%lu lum=[%u,%u]\n", kinds, min_lum, max_lum);
    /* lum = R*2 + G + B/2, 取值 0..140 (5/6/5 位) */
    check(kinds > 16, "颜色种类太少, 可能是纯色/未生成");
    check(min_lum < 40, "没有暗部, 图片可能是空白");
    check(max_lum > 120, "没有亮部, 图片可能是全黑");

    printf("%s: %d checks, %d fail\n", (g_fail == 0) ? "PASS" : "FAIL", g_checks, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
