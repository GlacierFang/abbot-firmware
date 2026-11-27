/**
 * ui_shutdown_state.c —— 关机流程的状态机（纯 C，主机侧可单测）
 *
 * 契约: abbot/doc/02 §3.6 关机契约
 *
 * 为什么独立成纯 C 文件：关机是**不可逆**操作 —— 按错一次就没有回头路。
 * 转换规则（谁能进哪个状态、谁能退出哪个状态）必须能在主机上被逐条验证，
 * 不能只靠真机手点。所以这里不碰 Arduino / LVGL / HAL，只管状态。
 */
#include "ui_shutdown_state.h"

static ui_shutdown_state_t s_state = UI_SHUTDOWN_IDLE;

ui_shutdown_state_t ui_shutdown_state_get(void)
{
    return s_state;
}

void ui_shutdown_state_reset(void)
{
    s_state = UI_SHUTDOWN_IDLE;
}

bool ui_shutdown_state_arm(void)
{
    /* 已经在确认态或已在关机流程里 → 不重复进。 */
    if (s_state != UI_SHUTDOWN_IDLE) {
        return false;
    }
    s_state = UI_SHUTDOWN_ARMED;
    return true;
}

bool ui_shutdown_state_disarm(void)
{
    /* 只有确认态能取消；已在关机流程里不可回退。 */
    if (s_state != UI_SHUTDOWN_ARMED) {
        return false;
    }
    s_state = UI_SHUTDOWN_IDLE;
    return true;
}

bool ui_shutdown_state_confirm(void)
{
    /* 只有确认态能确认 —— 没进确认态就不许直接关机。 */
    if (s_state != UI_SHUTDOWN_ARMED) {
        return false;
    }
    s_state = UI_SHUTDOWN_RUNNING;
    return true;
}
