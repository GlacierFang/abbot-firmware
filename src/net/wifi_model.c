/*
 * wifi_model.c —— Wi-Fi 扫描结果解析 + 连接状态机（纯逻辑）
 *
 * 不依赖 Arduino / LVGL / ESP-IDF：主机侧可用 gcc 直接编译并跑测试。
 */

#include "wifi_model.h"

#include <stdio.h>
#include <string.h>

int wifi_model_signal_level(int32_t rssi)
{
    if (rssi > -60) {
        return WIFI_SIGNAL_STRONG;
    }
    if (rssi >= -75) {
        return WIFI_SIGNAL_MID;
    }
    return WIFI_SIGNAL_WEAK;
}

const char *wifi_model_level_text(int level)
{
    switch (level) {
    case WIFI_SIGNAL_STRONG:
        return "信号强";
    case WIFI_SIGNAL_MID:
        return "信号中";
    default:
        return "信号弱";
    }
}

/* 同名 SSID 只留信号最强的一个；整体按信号从强到弱排。 */
int wifi_model_sort_aps(WifiApInfo *aps, int count)
{
    if (aps == NULL || count <= 0) {
        return 0;
    }

    /* 1) 去重：同名保留 RSSI 更大的那条（稳定的插入排序，避免 qsort 的不稳定） */
    int keep = 0;
    for (int i = 0; i < count; i++) {
        bool dup = false;
        for (int j = 0; j < keep; j++) {
            if (strcmp(aps[j].ssid, aps[i].ssid) == 0) {
                dup = true;
                if (aps[i].rssi > aps[j].rssi) {
                    aps[j] = aps[i];
                }
                break;
            }
        }
        if (!dup) {
            aps[keep++] = aps[i];
        }
    }

    /* 2) 排序：信号强的在前；同强度按 SSID 字典序，保证显示稳定不跳来跳去 */
    for (int i = 1; i < keep; i++) {
        WifiApInfo key = aps[i];
        int j = i - 1;
        while (j >= 0) {
            int a_level = wifi_model_signal_level(aps[j].rssi);
            int b_level = wifi_model_signal_level(key.rssi);
            bool less = (a_level < b_level) ||
                        (a_level == b_level && aps[j].rssi < key.rssi) ||
                        (a_level == b_level && aps[j].rssi == key.rssi &&
                         strcmp(aps[j].ssid, key.ssid) > 0);
            if (!less) {
                break;
            }
            aps[j + 1] = aps[j];
            j--;
        }
        aps[j + 1] = key;
    }
    return keep;
}

void wifi_model_mark_saved(WifiApInfo *aps, int count, const char *saved_ssid)
{
    if (aps == NULL || saved_ssid == NULL || saved_ssid[0] == '\0') {
        return;
    }
    for (int i = 0; i < count; i++) {
        aps[i].is_saved = (strcmp(aps[i].ssid, saved_ssid) == 0);
    }
}

void wifi_model_format_ap(char *out, size_t cap, const WifiApInfo *ap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (ap == NULL) {
        out[0] = '\0';
        return;
    }

    const char *name = (ap->ssid[0] != '\0') ? ap->ssid : "(隐藏网络)";
    const char *level = wifi_model_level_text(wifi_model_signal_level(ap->rssi));

    if (ap->is_saved) {
        snprintf(out, cap, "★ %s [%s]%s", name, level, ap->is_encrypted ? " [加密]" : " [开放]");
    } else {
        snprintf(out, cap, "%s [%s]%s", name, level, ap->is_encrypted ? " [加密]" : " [开放]");
    }
}

/* ===================== 列表行文案（设计稿口径） ===================== */

#define WIFI_ROW_BARS       4   /* 信号格数 */
#define WIFI_ROW_SSID_MAX  18   /* SSID 最多显示的字节数, 超出截断加省略号 */

/** SSID 截断: 按字节截, 但不把一个 UTF-8 字符劈成两半。 */
static void wifi_row_cut_ssid(char *out, size_t cap, const char *ssid)
{
    size_t len = strlen(ssid);
    if (len <= WIFI_ROW_SSID_MAX) {
        snprintf(out, cap, "%s", ssid);
        return;
    }
    size_t keep = (size_t)WIFI_ROW_SSID_MAX - 3;   /* 给 "…" 留位置 */
    while (keep > 0 && (((unsigned char)ssid[keep] & 0xC0U) == 0x80U)) {
        keep--;   /* 落在 UTF-8 续字节上, 往前退一格 */
    }
    snprintf(out, cap, "%.*s…", (int)keep, ssid);
}

void wifi_model_format_row(char *out, size_t cap, const WifiApInfo *ap, int connected)
{
    if (out == NULL || cap == 0) {
        return;
    }
    if (ap == NULL) {
        out[0] = '\0';
        return;
    }

    /* 信号格: 实心 █ 表示有信号, 浅底 ░ 表示这格没信号。
     * 字库里确定存在这两个字形 (tools/font_charset.txt)。 */
    char bars[WIFI_ROW_BARS * 3 + 1];
    int level = wifi_model_signal_level(ap->rssi);
    int filled = level + 1;
    if (filled < 1) {
        filled = 1;
    }
    if (filled > WIFI_ROW_BARS) {
        filled = WIFI_ROW_BARS;
    }
    int i;
    for (i = 0; i < WIFI_ROW_BARS; i++) {
        if (i < filled) {
            memcpy(bars + i * 3, "\xE2\x96\x88", 3);   /* █ */
        } else {
            memcpy(bars + i * 3, "\xE2\x96\x91", 3);   /* ░ */
        }
    }
    bars[WIFI_ROW_BARS * 3] = '\0';

    char name[WIFI_MODEL_SSID_MAX];
    const char *raw = (ap->ssid[0] != '\0') ? ap->ssid : "(隐藏网络)";
    wifi_row_cut_ssid(name, sizeof(name), raw);

    (void)connected;   /* 已连/已存/锁 移到行尾元信息列（见 wifi_model_format_meta） */

    snprintf(out, cap, "%s %s", bars, name);
}

void wifi_model_format_meta(char *out, size_t cap, const WifiApInfo *ap, int connected)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (ap == NULL) {
        return;
    }
    if (connected) {
        snprintf(out, cap, "已连");
    } else if (ap->is_saved) {
        snprintf(out, cap, "已存");
    } else if (ap->is_encrypted) {
        snprintf(out, cap, "锁");
    }
}

/* ===================== 连接阶段状态机 ===================== */

void wifi_conn_state_init(wifi_conn_state_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->phase = WIFI_CONN_IDLE;
    st->attempt = 0;
    st->max_attempt = WIFI_CONNECT_MAX_ATTEMPT;
    st->result_count = 0;
}

void wifi_conn_scan_start(wifi_conn_state_t *st, uint32_t now_ms)
{
    if (st == NULL) {
        return;
    }
    st->phase = WIFI_CONN_SCANNING;
    st->started_ms = now_ms;
    st->timeout_ms = WIFI_SCAN_TIMEOUT_MS;
}

void wifi_conn_scan_finish(wifi_conn_state_t *st, int count)
{
    if (st == NULL) {
        return;
    }
    st->phase = WIFI_CONN_IDLE;
    st->result_count = count;
}

bool wifi_conn_scan_expired(const wifi_conn_state_t *st, uint32_t now_ms)
{
    if (st == NULL || st->phase != WIFI_CONN_SCANNING) {
        return false;
    }
    return (uint32_t)(now_ms - st->started_ms) > st->timeout_ms;
}

void wifi_conn_connect_start(wifi_conn_state_t *st, const char *ssid, uint32_t now_ms)
{
    if (st == NULL) {
        return;
    }
    st->phase = WIFI_CONN_CONNECTING;
    st->started_ms = now_ms;
    st->timeout_ms = WIFI_CONNECT_TIMEOUT_MS;
    st->attempt++;
    if (ssid != NULL) {
        snprintf(st->ssid, sizeof(st->ssid), "%s", ssid);
    }
}

void wifi_conn_connect_finish(wifi_conn_state_t *st, bool ok)
{
    if (st == NULL) {
        return;
    }
    st->phase = ok ? WIFI_CONN_CONNECTED : WIFI_CONN_FAILED;
}

bool wifi_conn_connect_expired(const wifi_conn_state_t *st, uint32_t now_ms)
{
    if (st == NULL || st->phase != WIFI_CONN_CONNECTING) {
        return false;
    }
    return (uint32_t)(now_ms - st->started_ms) > st->timeout_ms;
}

bool wifi_conn_can_retry(const wifi_conn_state_t *st)
{
    if (st == NULL) {
        return false;
    }
    if (st->phase != WIFI_CONN_FAILED) {
        return false;
    }
    return st->attempt < st->max_attempt;
}

const char *wifi_conn_hint(const wifi_conn_state_t *st)
{
    if (st == NULL) {
        return "正在准备";
    }
    switch (st->phase) {
    case WIFI_CONN_SCANNING:
        return "正在搜索附近的无线网络…";
    case WIFI_CONN_CONNECTING:
        return "正在连接，请稍候…";
    case WIFI_CONN_CONNECTED:
        return "已连上无线网络";
    case WIFI_CONN_FAILED:
        return wifi_conn_can_retry(st) ? "没连上，可以再试一次"
                                        : "没连上，请核对密码或换一个网络";
    default:
        return "正在准备";
    }
}
