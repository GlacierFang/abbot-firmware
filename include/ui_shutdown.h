/**
 * ui_shutdown.h —— 合法关机流程（系统菜单入口 → 长按摇杆确认 → 深度睡眠）
 *
 * 硬件事实（abbot/PLAN-boot-screen.md）：
 *   - 板上没有 PMIC，也没有独立的电源切断电路，软件做不出真正断电。
 *     本流程能做到的最深状态是**深度睡眠 (deep sleep)**：主控与外设全停，
 *     静耗降到 µA 级；彻底断电仍然要拔电池或断充电。
 *   - 唤醒源只有摇杆中心键 GPIO0（下压，低有效），由 hal_power 配 ext0 唤醒。
 *   - 深睡会清掉 RAM 与外设状态，所以关机前必须把要留的东西交代好。
 *
 * 交互（doc/03 §五 系统设置第 10 项「关机」）：
 *   系统设置里光标停在「关机」项 -> 短按 = 进入待确认态（该行文案变成
 *   「关机（长按确认）」）
 *   -> 长按摇杆确认（长按在松开瞬间生效，doc/03 §3.2）
 *   -> 收尾（停录音、停播放、停 Wi-Fi、落盘）
 *   -> 显示「正在关机…」 -> 背光平滑淡出 -> 延时约 1 秒 -> 深度睡眠
 *   待确认态里再短按一下「关机」= 取消，文案还原成「关机」。
 *   待确认态只存在于系统设置页：一旦离开该页自动取消（由 ui_router 收尾）。
 *
 * 状态机在 ui_shutdown_state.c（纯 C，主机侧可单测）：
 *   IDLE -> ARMED（待确认）-> RUNNING（关机流程中，不可回退）
 */
#ifndef UI_SHUTDOWN_H
#define UI_SHUTDOWN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 系统设置里选中「关机」：进入待确认态（该行文案变成「关机（长按确认）」）。 */
void ui_shutdown_request(void);

/** 取消待确认态：文案还原成「关机」。 */
void ui_shutdown_cancel(void);

/** 是否处于待确认态（等长按确认）。 */
bool ui_shutdown_armed(void);

/** 是否正在执行关机流程（已确认，屏幕显示「正在关机…」）。 */
bool ui_shutdown_running(void);

/**
 * 确认关机：开始收尾流程。
 * 之后由 ui_shutdown_tick() 按时间推进到淡出与深睡。
 */
void ui_shutdown_confirm(void);

/**
 * 推进关机时序（界面核每 5ms 调一次，now_ms 为毫秒计数）。
 * 界面核在跑，所以「正在关机…」这一帧能真的刷到屏上。
 */
void ui_shutdown_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* UI_SHUTDOWN_H */
