/*
 * ui_error.cpp —— 错误码到中文文案的唯一查表处
 *
 * 文案逐条抄自 abbot/doc/03 §四, 一字不改。
 * 静默类错误不返回任何文案。
 */
#include "ui_error.h"

#include "ui_pages.h"
#include "ui_router.h"

typedef struct {
    const char *text;
    err_show_mode_t mode;
} ui_error_entry_t;

static const ui_error_entry_t s_table[ERR_COUNT] = {
    [ERR_NONE]            = { "",                                 ERR_SHOW_SILENT },
    [ERR_AUTH_4001]       = { "设备凭证已过期，请联网校准时间", ERR_SHOW_CARD },
    [ERR_CLOCK_NEED_SYNC] = { "设备需联网校准时间", ERR_SHOW_CARD },
    [ERR_KEY_4002]       = { "还没接上学习助手，请联网重试",                       ERR_SHOW_TOAST },
    [ERR_ASR_4003]        = { "没听清，请靠近麦克风重试",           ERR_SHOW_TOAST },
    [ERR_RATE_4290]       = { "老师正在思考，请稍候",               ERR_SHOW_TOAST },
    [ERR_QUOTA_4291]      = { "今日深度导学次数已用完",   ERR_SHOW_CARD },
    [ERR_SERVER_5001]     = { "云端异常，题目已存入待问收件箱",         ERR_SHOW_CARD },
    [ERR_WIFI_AUTH_FAIL]  = { "无线网络连接失败，请核对密码",               ERR_SHOW_TOAST },
    [ERR_NTP_SYNC_FAIL]   = { "自动校时未成功，请在设置中手动校时",         ERR_SHOW_TOAST },
    [ERR_CAM_AF_FAIL]     = { "摄像头对焦失败，请保持平稳后重拍",           ERR_SHOW_TOAST },
    [ERR_TTS_FALLBACK]    = { "",                                 ERR_SHOW_SILENT },
    [ERR_PACK_CORRUPT]    = { "学科包校验失败，请重新同步",         ERR_SHOW_CARD },
    [ERR_TOC_INVALID]     = { "",                                 ERR_SHOW_SILENT },
    [ERR_OTA_LOW_BAT]     = { "电量不足 35%，请充电后再升级",       ERR_SHOW_TOAST },
    [ERR_OTA_LOW_RSSI]    = { "无线信号过弱，请靠近路由器后升级",       ERR_SHOW_TOAST },
    [ERR_OTA_VERIFY_FAIL] = { "固件校验失败，升级已取消",   ERR_SHOW_CARD },
    [ERR_NO_TF_CARD]      = { "未检测到存储卡，请插入后使用",           ERR_SHOW_RESIDENT },
    [ERR_DISK_FULL]       = { "存储空间已满，请清理后重试",           ERR_SHOW_TOAST },
    [ERR_LOW_BAT_WARN]    = { "电量偏低，请及时充电",                       ERR_SHOW_TOAST },
    [ERR_LOW_BAT_SLEEP]   = { "电量将耗尽，设备进入休眠保护，充电后长按摇杆唤醒", ERR_SHOW_FULLSCREEN },
    [ERR_PSRAM_OOM]       = { "内存不足，请稍后重试",                         ERR_SHOW_TOAST },
    [ERR_HW_INIT_FAIL]    = { "硬件自检失败，请检查排线后重启",         ERR_SHOW_FULLSCREEN },
};

const char *ui_error_text(ui_error_code_t code)
{
    if (code >= ERR_COUNT) {
        return "设备异常，请稍后重试";
    }
    return s_table[code].text;
}

err_show_mode_t ui_error_mode(ui_error_code_t code)
{
    if (code >= ERR_COUNT) {
        return ERR_SHOW_TOAST;
    }
    return s_table[code].mode;
}

bool ui_show_friendly_error(ui_error_code_t code)
{
    err_show_mode_t mode = ui_error_mode(code);
    const char *text = ui_error_text(code);

    switch (mode) {
    case ERR_SHOW_SILENT:
        return false;

    case ERR_SHOW_CARD:
    case ERR_SHOW_RESIDENT:
    case ERR_SHOW_FULLSCREEN:
        /* 提示卡片承载: 短按关闭后恢复原页 (doc/03 §3.1) */
        ui_overlay_set_text(OVERLAY_REMINDER, text);
        ui_overlay_push(OVERLAY_REMINDER);
        return true;

    case ERR_SHOW_TOAST:
    default:
        ui_overlay_set_text(OVERLAY_TOAST, text);
        ui_overlay_push(OVERLAY_TOAST);
        return true;
    }
}
