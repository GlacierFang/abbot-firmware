#!/usr/bin/env python3
"""把开机图 PNG 转成固件里的 RGB565 静态数组。

用法:
    python3 tools/gen_boot_image.py <src.png> <out.c>

约定:
  * 源图必须能被缩放成 LCD_WIDTH x LCD_HEIGHT (320x480) —— 等比缩放 + 居中裁切。
  * RGBA 透明像素合成到白底 (255,255,255)。
  * 输出为 uint16_t 数组, **内存字节序 = RGB565 大端 (MSB 先行)**。
    ST7796S 走 SPI 按 MSB 收 16bit 色值; LVGL 侧 lv_conf.h 的
    LV_COLOR_16_SWAP=1 让 LVGL 的 color_t 也存成同样的字节序,
    所以本数组与 UI 走同一条 hal_display_blit_rgb565 通道, 颜色一致。
"""
import sys
from PIL import Image

W, H = 320, 480
BG = (255, 255, 255)


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    im = Image.open(src).convert("RGBA")
    # 等比缩放到覆盖整屏, 再居中裁切
    scale = max(W / im.width, H / im.height)
    im = im.resize((max(1, round(im.width * scale)), max(1, round(im.height * scale))), Image.LANCZOS)
    left = (im.width - W) // 2
    top = (im.height - H) // 2
    im = im.crop((left, top, left + W, top + H))
    flat = im.load()

    vals = []
    for y in range(H):
        for x in range(W):
            r, g, b, a = flat[x, y]
            if a != 255:
                # alpha 合成到白底
                r = (r * a + BG[0] * (255 - a)) // 255
                g = (g * a + BG[1] * (255 - a)) // 255
                b = (b * a + BG[2] * (255 - a)) // 255
            v = rgb565(r, g, b)
            # 大端字节序: 低地址字节 = 高位字节
            vals.append(((v & 0xFF) << 8) | ((v >> 8) & 0xFF))

    # 探针: 固定位置的规范 RGB565 数值 (未做字节序转换)。
    # 主机单测用它们确定性校验内存字节序 —— 不靠「相邻像素是否平滑」这种启发式。
    probe_xy = [(0, 0), (160, 60), (160, 200), (160, 240), (160, 470), (319, 479)]
    probe_idx = []
    probe_val = []
    for (px, py) in probe_xy:
        r, g, b, a = flat[px, py]
        if a != 255:
            r = (r * a + BG[0] * (255 - a)) // 255
            g = (g * a + BG[1] * (255 - a)) // 255
            b = (b * a + BG[2] * (255 - a)) // 255
        probe_idx.append(py * W + px)
        probe_val.append(rgb565(r, g, b))

    lines = []
    for i in range(0, len(vals), 16):
        chunk = ", ".join("0x%04X" % v for v in vals[i:i + 16])
        lines.append("    " + chunk + ",")
    body = "\n".join(lines)

    with open(dst, "w", encoding="utf-8") as f:
        f.write(
            "/*\n"
            " * 开机画面 —— 由 tools/gen_boot_image.py 自动生成, **不要手改**。\n"
            " *\n"
            " * 源图: workspace 仓 abbot/ui/poweron.png\n"
            " * 尺寸: %dx%d, RGB565, 内存字节序为大端 (MSB 先行, 与 ST7796S 一致)。\n"
            " * 重新生成:\n"
            " *   python3 tools/gen_boot_image.py ../ui/poweron.png src/ui/boot_image.c\n"
            " */\n"
            "#include \"ui_boot_image.h\"\n\n"
            "const uint16_t ui_boot_image[UI_BOOT_IMAGE_PIXELS] = {\n"
            "%s\n"
            "};\n"
            "\n"
            "/* 探针: 固定位置的规范 RGB565 数值 —— 主机单测用它确定性校验字节序。 */\n"
            "const uint32_t ui_boot_image_probe_idx[UI_BOOT_IMAGE_PROBE_COUNT] = {\n"
            "%s\n"
            "};\n"
            "\n"
            "const uint16_t ui_boot_image_probe_val[UI_BOOT_IMAGE_PROBE_COUNT] = {\n"
            "%s\n"
            "};\n" % (W, H, body,
                        "    " + ", ".join(str(i) for i in probe_idx) + ",",
                        "    " + ", ".join("0x%04X" % v for v in probe_val) + ",")
        )
    print("wrote %s: %d pixels (%d bytes)" % (dst, len(vals), len(vals) * 2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
