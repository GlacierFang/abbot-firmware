/**
 * ui_shutdown.cpp —— 关机流程的执行体（界面核 Core 1）
 *
 * 契约: abbot/doc/02 §3.6 关机契约、abbot/doc/03 §十 系统设置第 10 项
 *
 * 分工:
 *   - ui_shutdown_state.c（纯 C）管状态转换，主机侧可单测；
 *     本文件管**副作用**：停录音、停播放、停 Wi-Fi、落盘、刷屏、淡出、深睡。
 *   - 状态机管「能不能关机」，本文件管「关机怎么做」。
 *
 * 关机序列（用户长按确认之后）：
 *   停录音 -> 停播放并静音 -> 请求停 Wi-Fi -> 落盘会话状态
 *   -> 屏幕写「正在关机…」-> 背光平滑淡出 -> 延时约 1 秒 -> 深睡
 *
 * 深睡唤醒源只有摇杆中心键 GPIO0（hal_power_enter_deep_sleep 内部已配 ext0）。
 * 深睡会清掉 RAM 与外设状态，所以关机前该落盘的都在这里落盘了。
 */
#include <Arduino.h>

#include "hal_interfaces.h"
#include "net_api.h"
#include "ui_pages.h"
#include "ui_shutdown.h"
#include "ui_shutdown_state.h"
#include "voice_capture.h"

/* 阶段 */
enum {
    SHUTDOWN_STAGE_IDLE = 0,   /* 没在关机 */
    SHUTDOWN_STAGE_TEARDOWN,   /* 收尾中，等「正在关机…」上屏 */
    SHUTDOWN_STAGE_WAIT,       /* 背光已淡出，等深睡 */
};

/* 背光淡出步数：每步 5ms，30 步 ≈ 150ms。 */
#define SHUTDOWN_FADE_STEPS 30

/* 「正在关机…」上屏后至少停留这么久，让用户看清（LVGL 每 5ms 刷一帧）。 */
#define SHUTDOWN_MSG_HOLD_MS 500

/* 背光淡出后到深睡之间的延时（doc/02 §3.6）。 */
#define SHUTDOWN_SLEEP_DELAY_MS 1000

static int s_stage = SHUTDOWN_STAGE_IDLE;
static uint32_t s_stage_ms = 0;

void ui_shutdown_request(void)
{
    /* 已经在确认态或已经在关机流程里 → 不重复进。 */
    if (!ui_shutdown_state_arm()) {
        return;
    }
    /* 设置页那一行改成「关机（长按确认）」，用户看得见下一步怎么做。 */
    ui_pages_settings_shutdown_armed(true);
}

void ui_shutdown_cancel(void)
{
    if (!ui_shutdown_state_disarm()) {
        return;
    }
    ui_pages_settings_shutdown_armed(false);
}

bool ui_shutdown_armed(void)
{
    return ui_shutdown_state_get() == UI_SHUTDOWN_ARMED;
}

bool ui_shutdown_running(void)
{
    return ui_shutdown_state_get() == UI_SHUTDOWN_RUNNING;
}

void ui_shutdown_confirm(void)
{
    /* 只有确认态能确认 —— 状态机是唯一的把关点。 */
    if (!ui_shutdown_state_confirm()) {
        return;
    }
    s_stage = SHUTDOWN_STAGE_TEARDOWN;
    s_stage_ms = millis();

    /* 1. 停录音：UI 核只置标志，Core 0 的音频任务做真正的停止。 */
    voice_capture_cancel();

    /* 2. 停播放并静音功放：关机过程里不再出声。 */
    hal_audio_stop_playback();

    /* 3. 停 Wi-Fi：Core 0 网络任务执行（关射频）。
     *    NVS 里保存的 SSID/密码**不删** —— 深睡唤醒重启后照常自动重连。 */
    net_wifi_request_shutdown();

    /* 4. 状态落盘：当日阅读时长与阅读进度（doc/07 §2.3 / §2.4）。 */
    ui_pages_save_session_state();

    /* 5. 屏幕写「正在关机…」。 */
    ui_pages_settings_shutdown_armed(false);
    ui_pages_settings_shutdown_status("正在关机…");
}

void ui_shutdown_tick(uint32_t now_ms)
{
    if (!ui_shutdown_running()) {
        return;
    }
    switch (s_stage) {
    case SHUTDOWN_STAGE_TEARDOWN:
        /* 等「正在关机…」真的被刷到屏上，再开始淡出。 */
        if ((uint32_t)(now_ms - s_stage_ms) >= SHUTDOWN_MSG_HOLD_MS) {
            hal_display_fade_out_backlight(SHUTDOWN_FADE_STEPS);
            s_stage = SHUTDOWN_STAGE_WAIT;
            s_stage_ms = millis();
        }
        break;
    case SHUTDOWN_STAGE_WAIT:
        /* 屏幕已经全黑，停一下再深睡。 */
        if ((uint32_t)(now_ms - s_stage_ms) >= SHUTDOWN_SLEEP_DELAY_MS) {
            /* 深睡（唤醒源 = 摇杆中心键 GPIO0）；不返回。 */
            hal_power_enter_deep_sleep();
        }
        break;
    default:
        break;
    }
}
