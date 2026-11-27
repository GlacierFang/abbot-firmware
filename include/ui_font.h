/*
 * ui_font.h —— 界面用字库
 *
 * LVGL 内置的 Montserrat 没有汉字, 中文界面必须自带字库。
 * 这些字库是**裁剪版**: 只含界面实际用到的字符 (中文 + ASCII),
 * 由 Noto Sans CJK SC 裁剪生成, 4bpp。
 *
 * 分档 (2026-11-27): 原来只有 16px 一档, 标题/正文/状态行/提示全是同一个字号,
 * 层级只能靠位置和留白硬凑。分档后 (实际接线见 src/ui/ui_pages.cpp 的 ui_styles_init):
 *   - ui_font_cn_16  页面标题
 *   - ui_font_cn_12  正文 / 列表行 / 状态行 / 底部提示 / 行尾元信息
 *   - ui_font_cn_20  备用大字（顶栏等需要更醒目时用）
 *
 * 重新裁剪: firmware/tools/gen_font.sh (一次产出全部档位)
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 裁剪字库 (src/ui/fonts/ui_font_cn_<size>.c 由 tools/gen_font.sh 生成) */
extern const lv_font_t ui_font_cn_20;
extern const lv_font_t ui_font_cn_16;
extern const lv_font_t ui_font_cn_12;

#ifdef __cplusplus
}
#endif
