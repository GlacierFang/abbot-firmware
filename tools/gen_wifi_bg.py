#!/usr/bin/env python3
"""把首屏设计稿转成固件里的 RGB565 静态数组（LVGL 原生 16 位色内存布局）。

用法:
    python3 tools/gen_wifi_bg.py ../ui/wificonfig.png src/ui/wifi_bg.c

约定:
  * 源图按等比缩放 + 居中裁切成 320x480（与屏幕一致）。
  * 像素值按 **LVGL 原生 16 位色内存布局** 打包 —— 这正是 lv_conf.h 里
    LV_COLOR_DEPTH=16 / LV_COLOR_16_SWAP=1 时 lv_color_t 的位布局
    （见 LVGL v8 lv_color.h 的 lv_color16_t 与 LV_COLOR_MAKE16）：

        bit  0-2   green_h = (g >> 5) & 0x7
        bit  3-7   red     = (r >> 3) & 0x1F
        bit  8-12  blue    = (b >> 3) & 0x1F
        bit 13-15  green_l = (g >> 2) & 0x7

    这个数值与开机图（src/ui/boot_image.c）用的**完全相同** ——
    两边都是「规范 RGB565 的两个字节按 MSB 在前存放」，所以两张图经同一条
    hal_display_blit_rgb565 送屏时颜色一致。本数组给 LVGL 的
    lv_img_dsc_t（LV_IMG_CF_TRUE_COLOR）当页面背景用。
  * 输出为 uint16_t 数组 + 固定位置探针，主机单测用探针确定性校验位布局。
"""
import sys
from PIL import Image

W, H = 320, 480
BG = (255, 255, 255)


def pack_native(r, g, b):
    """按 LVGL lv_color16_t (LV_COLOR_16_SWAP=1) 的位布局打包 16 位色。"""
    green_h = (g >> 5) & 0x7
    red = (r >> 3) & 0x1F
    blue = (b >> 3) & 0x1F
    green_l = (g >> 2) & 0x7
    return green_h | (red << 3) | (blue << 8) | (green_l << 13)


def rgb565(r, g, b):
    """规范 RGB565（只做对照，测试里用它反推位布局）。"""
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGBA")
    scale = max(W / im.width, H / im.height)
    im = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)
    left = (im.width - W) // 2
    top = (im.height - H) // 2
    im = im.crop((left, top, left + W, top + H))
    flat = im.load()

    def px(x, y):
        r, g, b, a = flat[x, y]
        if a != 255:
            r = (r * a + BG[0] * (255 - a)) // 255
            g = (g * a + BG[1] * (255 - a)) // 255
            b = (b * a + BG[2] * (255 - a)) // 255
        return r, g, b

    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b = px(x, y)
            vals.append(pack_native(r, g, b))

    # 探针: 固定位置。val 用规范 RGB565 表达，测试里按位反推校验内存布局。
    probe_xy = [(0, 0), (160, 60), (160, 200), (160, 240), (160, 470), (319, 479)]
    probe_idx = []
    probe_val = []
    for (px_, py) in probe_xy:
        r, g, b = px(px_, py)
        probe_idx.append(py * W + px_)
        probe_val.append(rgb565(r, g, b))

    lines = []
    for i in range(0, len(vals), 16):
        chunk = ", ".join("0x%04X" % v for v in vals[i:i + 16])
        lines.append("    " + chunk + ",")
    body = "\n".join(lines)

    with open(dst, "w", encoding="utf-8") as f:
        f.write(
            "/*\n"
            " * 首屏背景图 —— 由 tools/gen_wifi_bg.py 自动生成, **不要手改**。\n"
            " *\n"
            " * 源图: workspace 仓 abbot/ui/wificonfig.png\n"
            " * 尺寸: %dx%d, RGB565, **LVGL 原生 16 位色内存布局**\n"
            " *       (LV_COLOR_16_SWAP=1: green_h|red<<3|blue<<8|green_l<<13)。\n"
            " * 重新生成:\n"
            " *   python3 tools/gen_wifi_bg.py ../ui/wificonfig.png src/ui/wifi_bg.c\n"
            " */\n"
            "#include \"ui_wifi_bg.h\"\n\n"
            "const uint16_t ui_wifi_bg[UI_WIFI_BG_PIXELS] = {\n"
            "%s\n"
            "};\n"
            "\n"
            "/* 探针: 固定位置的规范 RGB565 数值 —— 主机单测用它确定性校验位布局。 */\n"
            "const uint32_t ui_wifi_bg_probe_idx[UI_WIFI_BG_PROBE_COUNT] = {\n"
            "%s\n"
            "};\n"
            "\n"
            "const uint16_t ui_wifi_bg_probe_val[UI_WIFI_BG_PROBE_COUNT] = {\n"
            "%s\n"
            "};\n" % (W, H, body,
                        "    " + ", ".join(str(i) for i in probe_idx) + ",",
                        "    " + ", ".join("0x%04X" % v for v in probe_val) + ",")
        )
    print("wrote %s: %d pixels (%d bytes)" % (dst, len(vals), len(vals) * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
