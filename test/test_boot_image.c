/*
 * test_boot_image.c —— 开机画面位图的静态自检（主机侧）
 *
 * 检的是「这张图有没有被做坏」，不是「像素长什么样」——
 * 换设计稿时只要是正常的开机图, 这些断言就该继续成立:
 *   1. 尺寸常量与屏幕一致 (320x480), 数组长度对得上。
 *   2. 内存字节序是 MSB 先行 (大端): 用生成器写死的探针位置做**确定性**校验 ——
 *      数组里那个 uint16 的两个字节按「低地址在前」读出来, 应当正好等于
 *      该像素的规范 RGB565 数值。顺序写反了这条立刻炸, 不依赖任何启发式。
 *   3. 内容非退化: 颜色种类够多, 既有暗部也有亮部 (纯色/全黑/全白都要被挡住)。
 */
#include <stdio.h>
#include <string.h>

#include "ui_boot_image.h"

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
 * 数组内存字节序是大端 (低地址 = 高字节), 所以规范值 = 把两字节换回来。 */
static unsigned canonical_rgb565(uint16_t stored)
{
    return ((unsigned)(stored & 0xFF) << 8) | ((unsigned)(stored >> 8) & 0xFF);
}

int main(void)
{
    check(UI_BOOT_IMAGE_WIDTH == 320, "宽度必须是 320");
    check(UI_BOOT_IMAGE_HEIGHT == 480, "高度必须是 480");
    check(UI_BOOT_IMAGE_PIXELS == 320 * 480, "像素总数必须是 320*480");

    /* 字节序探针: 生成器把固定位置的规范 RGB565 写死在探针数组里 */
    for (int i = 0; i < UI_BOOT_IMAGE_PROBE_COUNT; i++) {
        uint32_t idx = ui_boot_image_probe_idx[i];
        check(idx < (uint32_t)UI_BOOT_IMAGE_PIXELS, "探针下标越界");
        unsigned got = canonical_rgb565(ui_boot_image[idx]);
        unsigned want = ui_boot_image_probe_val[i];
        if (got != want) {
            printf("FAIL: 探针 %d (idx=%u) 字节序不符: got 0x%04X want 0x%04X\n", i, idx, got, want);
            g_fail++;
            g_checks++;
        } else {
            g_checks++;
        }
    }

    /* 内容非退化 */
    unsigned distinct = 0;
    int has_dark = 0;
    int has_light = 0;
    unsigned prev = 0xFFFFFFFFu;
    for (int i = 0; i < UI_BOOT_IMAGE_PIXELS; i++) {
        unsigned v = ui_boot_image[i];
        if (v != prev) {
            distinct++;
            prev = v;
        }
        unsigned c = canonical_rgb565((uint16_t)v);
        unsigned r = ((c >> 11) & 0x1F) * 255u / 31u;
        unsigned g = ((c >> 5) & 0x3F) * 255u / 63u;
        unsigned b = (c & 0x1F) * 255u / 31u;
        unsigned l = (r * 30u + g * 59u + b * 11u) / 100u;
        if (l < 40) {
            has_dark = 1;
        }
        if (l > 200) {
            has_light = 1;
        }
    }
    check(distinct >= 64, "颜色种类过少, 图可能被压成了近似纯色");
    check(has_dark, "图里没有暗部, 疑似内容丢失");
    check(has_light, "图里没有亮部, 疑似内容丢失");

    printf("boot image: %dx%d, %u distinct colors, %d probes, %d checks, %d failures\n",
           UI_BOOT_IMAGE_WIDTH, UI_BOOT_IMAGE_HEIGHT, distinct,
           UI_BOOT_IMAGE_PROBE_COUNT, g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
