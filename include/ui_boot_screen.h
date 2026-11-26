/*
 * ui_boot_screen.h —— 开机画面
 *
 * 契约: abbot/doc/02 §3.1（ST7796S 竖屏 320×480）
 *
 * 上电时先整屏刷入开机图, 再逐档点亮背光 —— 用户上电第一眼看到的是开机图,
 * 而不是白屏/花屏。开机图是编译进固件的 RGB565 静态数组, 不依赖 TF 卡与网络。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 刷开机图（背光点亮之前调用）。内部保证不返回前屏幕已有内容。 */
void ui_boot_screen_show(void);

#ifdef __cplusplus
}
#endif
