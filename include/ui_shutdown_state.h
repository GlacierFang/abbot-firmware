/**
 * ui_shutdown_state.h —— 关机流程的状态机（纯 C，主机侧可单测）
 *
 * 为什么单独抽出来：关机是**不可逆**操作 —— 按错一次就没有回头路。
 * 状态转换规则必须能在主机上被逐条验证（test/test_ui_shutdown_state.c），
 * 不能只靠真机手点。
 *
 * 状态：
 *   IDLE    平时（不在关机流程里）
 *   ARMED   已在关机确认页，等摇杆长按；此时短按 / 其它手势 = 取消
 *   RUNNING 已确认，关机流程执行中（不可回退）
 *
 * 契约: abbot/doc/02 §3.6 关机契约、abbot/doc/03 §五 关机确认页
 */
#ifndef ABBOT_UI_SHUTDOWN_STATE_H
#define ABBOT_UI_SHUTDOWN_STATE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SHUTDOWN_IDLE = 0,
    UI_SHUTDOWN_ARMED,
    UI_SHUTDOWN_RUNNING,
} ui_shutdown_state_t;

/** 当前状态。 */
ui_shutdown_state_t ui_shutdown_state_get(void);

/** 回到 IDLE（开机初始化用）。 */
void ui_shutdown_state_reset(void);

/**
 * 进入确认态（IDLE -> ARMED）。
 * 只有 IDLE 能进；已在 ARMED / RUNNING 返回 false（不重复 arm）。
 */
bool ui_shutdown_state_arm(void);

/**
 * 取消确认态（ARMED -> IDLE）。
 * 只有 ARMED 能取消；IDLE / RUNNING 返回 false。
 */
bool ui_shutdown_state_disarm(void);

/**
 * 确认关机（ARMED -> RUNNING）。
 * 只有 ARMED 能确认；IDLE / RUNNING 返回 false。
 * 返回 true 表示**这次调用真的启动了关机流程** —— 调用方据此执行不可逆动作。
 */
bool ui_shutdown_state_confirm(void);

#ifdef __cplusplus
}
#endif

#endif /* ABBOT_UI_SHUTDOWN_STATE_H */
