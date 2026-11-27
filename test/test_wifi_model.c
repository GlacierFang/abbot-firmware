/*
 * test_wifi_model.c —— Wi-Fi 扫描结果解析 + 连接状态机的主机侧测试
 *
 * 编译: gcc -std=c11 -Wall -Wextra -Werror -Iinclude test/test_wifi_model.c \
 *           src/net/wifi_model.c -o /tmp/test_wifi_model
 * 运行: /tmp/test_wifi_model   (全部通过时打印 ALL PASS)
 *
 * 这个测试要证明的是：页面上显示的是**真扫描结果**（有排序、去重、信号分级、加密标记、
 * 已保存标记），连接状态机能走到「失败 → 可重试」，且用户可见文案里没有技术术语。
 */

#include <stdio.h>
#include <string.h>

#include "wifi_model.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expect)                                              \
    do {                                                                       \
        const char *a_ = (actual);                                             \
        const char *e_ = (expect);                                             \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                               \
            printf("FAIL %s:%d  got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                   a_ ? a_ : "(null)", e_);                                     \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

static void set_ap(WifiApInfo *ap, const char *ssid, int32_t rssi, bool enc)
{
    memset(ap, 0, sizeof(*ap));
    snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
    ap->rssi = rssi;
    ap->is_encrypted = enc;
    ap->is_saved = false;
}

static void test_signal_level(void)
{
    /* doc/03 §11: > -60 强；-60 ~ -75 中；< -75 弱 */
    CHECK(wifi_model_signal_level(-40) == WIFI_SIGNAL_STRONG);
    CHECK(wifi_model_signal_level(-59) == WIFI_SIGNAL_STRONG);
    CHECK(wifi_model_signal_level(-60) == WIFI_SIGNAL_MID);
    CHECK(wifi_model_signal_level(-70) == WIFI_SIGNAL_MID);
    CHECK(wifi_model_signal_level(-75) == WIFI_SIGNAL_MID);
    CHECK(wifi_model_signal_level(-76) == WIFI_SIGNAL_WEAK);
    CHECK(wifi_model_signal_level(-90) == WIFI_SIGNAL_WEAK);

    CHECK_STR(wifi_model_level_text(WIFI_SIGNAL_STRONG), "信号强");
    CHECK_STR(wifi_model_level_text(WIFI_SIGNAL_MID), "信号中");
    CHECK_STR(wifi_model_level_text(WIFI_SIGNAL_WEAK), "信号弱");
}

static void test_sort_and_dedupe(void)
{
    WifiApInfo aps[4];
    set_ap(&aps[0], "WeakNet", -85, false);
    set_ap(&aps[1], "StrongNet", -45, true);
    set_ap(&aps[2], "MidNet", -66, true);
    set_ap(&aps[3], "StrongNet", -70, false); /* 同名不同射频 */

    int n = wifi_model_sort_aps(aps, 4);
    /* 同名合并后剩 3 个 */
    CHECK(n == 3);
    /* 最强的排前面 */
    CHECK_STR(aps[0].ssid, "StrongNet");
    CHECK(aps[0].rssi == -45);
    /* 同名里保留的是信号最强的那条（加密标记也跟着 strongest 那条） */
    CHECK(aps[0].is_encrypted == true);
    /* 其余按信号强弱排 */
    CHECK_STR(aps[1].ssid, "MidNet");
    CHECK_STR(aps[2].ssid, "WeakNet");

    /* 空数组不炸 */
    CHECK(wifi_model_sort_aps(NULL, 0) == 0);
    CHECK(wifi_model_sort_aps(aps, 0) == 0);
}

static void test_mark_saved(void)
{
    WifiApInfo aps[2];
    set_ap(&aps[0], "Home", -50, true);
    set_ap(&aps[1], "Guest", -60, false);

    wifi_model_mark_saved(aps, 2, "Home");
    CHECK(aps[0].is_saved == true);
    CHECK(aps[1].is_saved == false);

    /* 空 SSID 不乱标 */
    wifi_model_mark_saved(aps, 2, "");
    CHECK(aps[0].is_saved == true);
}

static void test_format(void)
{
    char buf[96];

    WifiApInfo ap;
    set_ap(&ap, "Home_WiFi_5G", -50, true);
    wifi_model_format_ap(buf, sizeof(buf), &ap);
    /* 行文案里必须出现真实 SSID 与信号、加密标记 */
    CHECK(strstr(buf, "Home_WiFi_5G") != NULL);
    CHECK(strstr(buf, "信号强") != NULL);
    CHECK(strstr(buf, "加密") != NULL);
    CHECK(strstr(buf, "★") == NULL); /* 没保存就没有星号 */

    ap.is_saved = true;
    wifi_model_format_ap(buf, sizeof(buf), &ap);
    CHECK(strstr(buf, "★") != NULL);

    /* 开放网络 */
    set_ap(&ap, "OpenNet", -80, false);
    wifi_model_format_ap(buf, sizeof(buf), &ap);
    CHECK(strstr(buf, "开放") != NULL);
    CHECK(strstr(buf, "信号弱") != NULL);
    CHECK(strstr(buf, "加密") == NULL);

    /* 隐藏网络不能显示成空白 */
    set_ap(&ap, "", -50, true);
    wifi_model_format_ap(buf, sizeof(buf), &ap);
    CHECK(strstr(buf, "隐藏网络") != NULL);
}

static void test_conn_state_machine(void)
{
    wifi_conn_state_t st;
    wifi_conn_state_init(&st);
    CHECK(st.phase == WIFI_CONN_IDLE);
    CHECK(st.attempt == 0);
    CHECK(st.max_attempt == WIFI_CONNECT_MAX_ATTEMPT);

    /* 扫描阶段 */
    wifi_conn_scan_start(&st, 1000);
    CHECK(st.phase == WIFI_CONN_SCANNING);
    CHECK(wifi_conn_scan_expired(&st, 1000) == false);
    CHECK(wifi_conn_scan_expired(&st, 1000 + WIFI_SCAN_TIMEOUT_MS) == false);
    CHECK(wifi_conn_scan_expired(&st, 1000 + WIFI_SCAN_TIMEOUT_MS + 1) == true);

    wifi_conn_scan_finish(&st, 3);
    CHECK(st.phase == WIFI_CONN_IDLE);
    CHECK(st.result_count == 3);

    /* 扫不到也是正常结束 */
    wifi_conn_scan_start(&st, 2000);
    wifi_conn_scan_finish(&st, 0);
    CHECK(st.result_count == 0);

    /* 连接阶段 */
    wifi_conn_connect_start(&st, "Home", 3000);
    CHECK(st.phase == WIFI_CONN_CONNECTING);
    CHECK(st.attempt == 1);
    CHECK_STR(st.ssid, "Home");
    CHECK(wifi_conn_connect_expired(&st, 3000 + WIFI_CONNECT_TIMEOUT_MS) == false);
    CHECK(wifi_conn_connect_expired(&st, 3000 + WIFI_CONNECT_TIMEOUT_MS + 1) == true);

    /* 失败 → 可重试 */
    wifi_conn_connect_finish(&st, false);
    CHECK(st.phase == WIFI_CONN_FAILED);
    CHECK(wifi_conn_can_retry(&st) == true);

    /* 重试到上限 → 不再提示重试，改成核对密码 */
    wifi_conn_connect_start(&st, "Home", 4000);
    wifi_conn_connect_finish(&st, false);
    CHECK(st.attempt == 2);
    CHECK(wifi_conn_can_retry(&st) == true);
    wifi_conn_connect_start(&st, "Home", 5000);
    wifi_conn_connect_finish(&st, false);
    CHECK(st.attempt == 3);
    CHECK(wifi_conn_can_retry(&st) == false);

    /* 成功 */
    wifi_conn_connect_start(&st, "Home", 6000);
    wifi_conn_connect_finish(&st, true);
    CHECK(st.phase == WIFI_CONN_CONNECTED);
    CHECK(wifi_conn_can_retry(&st) == false);
}

static bool has_tech_word(const char *s)
{
    static const char *bad[] = { "HTTP", "AES", "signature", "Token", "Python", "Traceback",
                                 "ssid", "password", "rssi", "nvs", "WiFi", NULL };
    for (int i = 0; bad[i] != NULL; i++) {
        if (strstr(s, bad[i]) != NULL) {
            return true;
        }
    }
    return false;
}

static void test_hint_is_user_friendly(void)
{
    wifi_conn_state_t st;
    wifi_conn_state_init(&st);

    wifi_conn_scan_start(&st, 0);
    CHECK(has_tech_word(wifi_conn_hint(&st)) == false);

    wifi_conn_connect_start(&st, "Home", 0);
    CHECK(has_tech_word(wifi_conn_hint(&st)) == false);

    wifi_conn_connect_finish(&st, false);
    CHECK(has_tech_word(wifi_conn_hint(&st)) == false);

    wifi_conn_connect_finish(&st, true);
    CHECK(has_tech_word(wifi_conn_hint(&st)) == false);
    CHECK(strstr(wifi_conn_hint(&st), "已连上") != NULL);
}

/* 列表行文案（设计稿口径）—— 信号格 / SSID 截断 / 锁标 / 已连已存 */
static void test_format_row(void)
{
    struct WifiApInfo ap;
    char out[128];
    char meta[16];

    memset(&ap, 0, sizeof(ap));
    snprintf(ap.ssid, sizeof(ap.ssid), "%s", "Home_WiFi_5G");
    ap.rssi = -40;                 /* 强 */
    ap.is_encrypted = true;
    ap.is_saved = false;

    wifi_model_format_row(out, sizeof(out), &ap, 0);
    CHECK(strstr(out, "Home_WiFi_5G") != NULL);
    CHECK(strstr(out, "锁") == NULL);       /* 锁标已移到行尾元信息列 */

    wifi_model_format_meta(meta, sizeof(meta), &ap, 0);
    CHECK(strstr(meta, "锁") != NULL);      /* 加密网络标锁 */
    CHECK(strstr(meta, "已存") == NULL);    /* 未保存且未连接 */

    wifi_model_format_meta(meta, sizeof(meta), &ap, 1);
    CHECK(strstr(meta, "已连") != NULL);    /* 已连接 */

    /* 弱信号：实心格变少 */
    ap.rssi = -85;
    wifi_model_format_row(out, sizeof(out), &ap, 0);
    CHECK(strstr(out, "█") != NULL);

    /* SSID 超长（ASCII）：截断到 15 字节 + 省略号 */
    memset(ap.ssid, 0, sizeof(ap.ssid));
    memset(ap.ssid, 'A', 30);
    ap.ssid[30] = '\0';
    ap.rssi = -40;
    wifi_model_format_row(out, sizeof(out), &ap, 0);
    CHECK(strstr(out, "…") != NULL);
    CHECK(strstr(out, "AAAAAAAAAAAAAAA…") != NULL);
    CHECK(strstr(out, "AAAAAAAAAAAAAAAA") == NULL);

    /* SSID 超长（UTF-8）：截断后不能劈开一个汉字 */
    memset(ap.ssid, 0, sizeof(ap.ssid));
    for (int i = 0; i < 10; i++) {
        memcpy(ap.ssid + i * 3, "一", 3);
    }
    ap.ssid[30] = '\0';
    wifi_model_format_row(out, sizeof(out), &ap, 0);
    CHECK(strstr(out, "…") != NULL);
    CHECK(strstr(out, "一一一一一…") != NULL);
    CHECK(strstr(out, "一一一一一一") == NULL);
}

int main(void)
{
    test_signal_level();
    test_sort_and_dedupe();
    test_mark_saved();
    test_format();
    test_format_row();
    test_conn_state_machine();
    test_hint_is_user_friendly();

    if (g_fail == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILED\n", g_fail);
    return 1;
}
