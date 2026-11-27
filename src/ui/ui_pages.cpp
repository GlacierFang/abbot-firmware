/*
 * ui_pages.cpp —— 14 个核心页面 + 电子书子页面 + 三层浮层的 LVGL 骨架
 *
 * 契约来源: abbot/doc/03 §二 三区骨架 / §三 浮层优先级 / §五 14 页线框,
 *           abbot/doc/07 电子书子页面。
 *
 * 设计约定:
 *   - 本文件只负责**建页面**与**画内容**, 不做路由决策、不计时。
 *   - 路由决策在 ui_router.cpp; 手势的短按/长按判定由 HAL 裁定后传入。
 *   - 严禁在本文件里出现任何阻塞式网络 / TF 卡 IO —— 那是 task_net_worker / task_storage 的活。
 *   - 严禁出现 AES / HTTP / signature / Token / Python / Traceback 等技术术语字样。
 */
#include "ui_pages.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "ui_font.h"

#include "hal_interfaces.h"
#include "keypad_editor.h"
#include "net_api.h"
#include "ui_book_guard.h"
#include "ui_data.h"
#include "ui_ota_page.h"
#include "ui_weather.h"
#include "ui_wifi_bg.h"
#include "wifi_model.h"

/* ===================== 颜色（中性骨架，落地前按 doc/03 §一 核对） ===================== */
#define UI_C_BG        lv_color_hex(0xFFFFFF)
#define UI_C_CARD      lv_color_hex(0xF2F4F7)
#define UI_C_LINE      lv_color_hex(0xD7DCE3)
#define UI_C_TEXT      lv_color_hex(0x1F2430)
#define UI_C_SUBTEXT   lv_color_hex(0x6B7280)
#define UI_C_ACCENT    lv_color_hex(0x2F6FED)
#define UI_C_WARN      lv_color_hex(0xB42318)

/* ===================== 三区几何（doc/03 §二） ===================== */
#define UI_STATUS_H    24
#define UI_HINT_H      32
#define UI_CONTENT_Y   UI_STATUS_H
#define UI_CONTENT_H   (480 - UI_STATUS_H - UI_HINT_H)   /* 424 */
#define UI_PAD         8

static lv_style_t s_style_root;
static lv_style_t s_style_card;
static lv_style_t s_style_title;
static lv_style_t s_style_body;
static lv_style_t s_style_hint;
static lv_style_t s_style_status;
static bool s_styles_ready = false;

/* 页面内容对象句柄, 每个页面一个 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *status;
    lv_obj_t *status_label;
    lv_obj_t *content;
    lv_obj_t *hint;
} ui_page_widgets_t;

static ui_page_widgets_t s_pages[PAGE_COUNT];
static ui_page_widgets_t s_book_pages[PAGE_BOOK_COUNT - PAGE_BOOKSHELF];

/* ===================== 列表光标（doc/03 §三） ===================== */

#define UI_MAX_ITEMS 12          /* 每页最多可选行数 */
#define UI_ITEM_TEXT_MAX 72      /* 行文案（不含行首标记）上限 */
#define UI_SOLVE_TEXT_MAX 1024   /* 解析正文缓存上限 */
#define UI_SOLVE_LINES 13        /* 正文一屏显示的行数 */

typedef struct {
    lv_obj_t *row;
    lv_obj_t *label;
    char text[UI_ITEM_TEXT_MAX]; /* 去掉行首 "> " / "  " 标记后的原文 */
} ui_item_t;

static ui_item_t s_items[PAGE_COUNT][UI_MAX_ITEMS];
static uint8_t s_item_count[PAGE_COUNT];
static uint8_t s_cursor[PAGE_COUNT];
static int s_build_page = -1;     /* 正在构建的页面（-1 = 不登记条目） */

/* 光标渲染（定义在文件后面的「列表光标」段 —— 这里先前置声明） */
static void ui_cursor_render(ui_page_t page);

/* 解析结果页的动态内容 */
static lv_obj_t *s_solve_body = NULL;
static lv_obj_t *s_solve_question = NULL;
static char s_solve_text[UI_SOLVE_TEXT_MAX];
static int s_solve_scroll = 0;

/* ===================== 番茄钟（doc/03 §五 第 8 页） ===================== */

typedef enum {
    POMO_IDLE = 0,
    POMO_RUNNING,
    POMO_PAUSED,
    POMO_DONE
} pomo_state_t;

static pomo_state_t s_pomo_state = POMO_IDLE;
static uint32_t s_pomo_total_ms = 0;
static uint32_t s_pomo_remain_ms = 0;
static uint32_t s_pomo_last_ms = 0;
static bool s_pomo_tick_ready = false;
static lv_obj_t *s_pomo_time_label = NULL;
static lv_obj_t *s_pomo_sub_label = NULL;

/* 专注时长（分钟）：默认 25，可调，持久化在 TF 卡 /config/settings.json */
static int s_focus_minutes = UI_DATA_FOCUS_DEFAULT;
static bool s_focus_loaded = false;
static lv_obj_t *s_focus_value_label = NULL;

/* 首页「今日课程」卡片 */
static lv_obj_t *s_home_schedule_label = NULL;
/* 首页天气标签（真实数据：服务端应答里的 temp + text；空态显示温和提示） */
static lv_obj_t *s_home_weather_label = NULL;
/* 设置页「所在城市」右侧的值标签 */
static lv_obj_t *s_city_value_label = NULL;

/* ===================== 学科卡片（doc/04 §四） ===================== */

#define UI_MAX_PACKS   8   /* 学科包列表最多显示几个 */
#define UI_PACK_ID_MAX 32  /* 学科包 id 长度上限 */

static const card_pack_t *s_cards = NULL;   /* 已载入的卡片集（UI 持有指针） */
static int s_card_index = 0;                /* 当前这张卡（0 起） */
static bool s_card_show_answer = false;     /* 当前是不是背面 */
static lv_obj_t *s_card_label = NULL;       /* 卡片正文标签 */
static lv_obj_t *s_card_pos_label = NULL;   /* 「第 N / M 张」 */

/* 学科包列表（读卡上 /subjects 下的真实目录） */
static char s_pack_ids[UI_MAX_PACKS][UI_PACK_ID_MAX];
static int s_pack_count = 0;

/* OOBE 自动接入进度页（doc/04 §1.2 自动登记） */
static lv_obj_t *s_oobe_state_label = NULL;

/* ===================== 书架与阅读页（doc/07） ===================== */

#define UI_MAX_BOOKS       8    /* 书架最多列出的书 */
#define UI_MAX_CHAPTERS    128  /* 章节索引上限（每章 8 字节索引） */
#define UI_READER_HISTORY  16   /* 往回翻记住的页起点数 */

static char s_book_names[UI_MAX_BOOKS][UI_DATA_MAX_NAME];
static uint32_t s_book_sizes[UI_MAX_BOOKS];
static int s_book_count = 0;
static int s_book_cursor = 0;
static lv_obj_t *s_book_labels[UI_MAX_BOOKS];

static char s_reader_path[UI_DATA_MAX_PATH];
static uint32_t s_reader_offset = 0;
static uint32_t s_reader_consumed = 0;
static uint32_t s_reader_size = 0;

/* ===================== 阅读子页状态（菜单 / 目录 / 跳转） =====================
 *
 * 三个子页共用同一套结构：进页重建 → 手势改状态 → 再重建。
 *   PAGE_BOOK_MENU  快捷菜单（目录 / 百分比定位 / 亮度 / 返回书架）
 *   PAGE_BOOK_TOC   章节目录（8 章一页，左右翻页，短按跳章）
 *   PAGE_BOOK_JUMP  百分比定位（左右 ±1%，上下 ±10%，短按确认跳转）
 * 跳转落点一律走 ui_data_percent_to_offset() + UTF-8 首字节对齐（doc/07 §3.4）。
 * ================================================================================== */
#define UI_TOC_PER_PAGE   8     /* 目录页每页章数（doc/07 §4.2 ④） */
#define UI_TOC_TITLE_MAX  32    /* 单个章节标题最长字节 */
#define UI_JUMP_BLOCKS    16    /* 跳转页进度条格数（16 全角格 = 256px，容得下） */
#define UI_MENU_ROWS      4     /* 菜单最多 4 项（doc/07 §4.2 ③） */

static int s_book_menu_sel = 0;    /* 菜单光标 */
static int s_book_toc_sel = 0;     /* 目录选中章（全局索引） */
static int s_book_toc_page = 0;    /* 目录当前页 */
static int s_book_jump_pct = 0;    /* 跳转目标百分比 */

/* 菜单项动作码（定义在 ui_pages.h，ui_router 依此跳转） */
static int s_menu_actions[UI_MENU_ROWS];
static int s_menu_count = 0;

/* 章节索引：只存两样 —— 每章在正文里的字节偏移、每章在 .toc 里的行首位置。
 * 章节标题**不常驻内存**：显示哪一页，才把那 8 行按位置读回来
 * （doc/07 §4.2 ④「每页显示 8 章，流式局部读取」）。 */
static uint32_t s_chapter_offsets[UI_MAX_CHAPTERS];
static uint32_t s_chapter_toc_pos[UI_MAX_CHAPTERS];
static int s_chapter_count = 0;
static char s_chapter_title[UI_TOC_TITLE_MAX];   /* 正文状态栏用的当前章节名 */
static int s_chapter_title_idx = -1;
static char s_toc_path[UI_DATA_MAX_PATH];
static uint32_t s_toc_size = 0;

/* .toc 流式读取的缓冲（静态，不占 UI 任务栈） */
static uint8_t s_toc_chunk[256];
static char s_toc_line[96];
static uint8_t s_toc_readbuf[1024];

static char s_toc_titles[UI_TOC_PER_PAGE][UI_TOC_TITLE_MAX];  /* 当前页标题缓存 */
static int s_toc_page_cached = -1;                            /* 标题缓存对应的页号 */
static uint32_t s_reader_history[UI_READER_HISTORY];
static int s_reader_history_count = 0;
static lv_obj_t *s_reader_body = NULL;

/* ===================== 阅读护栏（doc/07 §2.3 / §2.4） ===================== */

static ui_bookmarks_t s_bookmarks;           /* /records/bookmarks.json 的内存形态 */
static bool s_bookmarks_loaded = false;
static ui_book_guard_t s_book_guard;         /* /config/profile.json 的护栏配置 */
static bool s_book_guard_loaded = false;
static uint32_t s_reader_tick_last_ms = 0;   /* 上一次计时的毫秒计数 */
static uint32_t s_reader_session_ms = 0;     /* 本次会话的零头（不足 1 秒的部分） */
static bool s_reader_counting = false;       /* 是否正在计当日阅读时长 */
static lv_obj_t *s_locked_title = NULL;      /* 宵禁 / 上限提示页 */
static lv_obj_t *s_locked_body = NULL;

/* 进度表读写用的缓冲（静态：不占 UI 任务栈） */
static char s_bookmarks_buf[1024];
static char s_bookmarks_bak_buf[1024];
static char s_bookmarks_out[1400];

/* 浮层句柄 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *label;
} ui_overlay_widgets_t;

static ui_overlay_widgets_t s_overlays[OVERLAY_COUNT];

/* ===================== 样式 ===================== */
static void ui_styles_init(void)
{
    if (s_styles_ready) {
        return;
    }

    lv_style_init(&s_style_root);
    lv_style_set_bg_color(&s_style_root, UI_C_BG);
    lv_style_set_bg_opa(&s_style_root, LV_OPA_COVER);
    lv_style_set_pad_all(&s_style_root, 0);
    lv_style_set_border_width(&s_style_root, 0);

    lv_style_init(&s_style_card);
    lv_style_set_bg_color(&s_style_card, UI_C_CARD);
    lv_style_set_bg_opa(&s_style_card, LV_OPA_COVER);
    lv_style_set_radius(&s_style_card, 6);
    lv_style_set_border_width(&s_style_card, 1);
    lv_style_set_border_color(&s_style_card, UI_C_LINE);
    lv_style_set_pad_all(&s_style_card, UI_PAD);

    lv_style_init(&s_style_title);
    lv_style_set_text_color(&s_style_title, UI_C_TEXT);
    lv_style_set_text_font(&s_style_title, &ui_font_cn_16);

    lv_style_init(&s_style_body);
    lv_style_set_text_color(&s_style_body, UI_C_TEXT);
    lv_style_set_text_font(&s_style_body, &ui_font_cn_16);

    lv_style_init(&s_style_hint);
    lv_style_set_text_color(&s_style_hint, UI_C_SUBTEXT);
    lv_style_set_text_font(&s_style_hint, &ui_font_cn_16);

    lv_style_init(&s_style_status);
    lv_style_set_text_color(&s_style_status, UI_C_SUBTEXT);
    lv_style_set_text_font(&s_style_status, &ui_font_cn_16);

    s_styles_ready = true;
}

/* ===================== 通用构件 ===================== */

static lv_obj_t *ui_make_root(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    /* LVGL 8.x 没有 lv_obj_set_style_all (那是 v9 的 API) —— 等价写法是
     * 先清掉默认样式再挂自己的。 */
    lv_obj_remove_style_all(root);
    lv_obj_add_style(root, &s_style_root, LV_PART_MAIN);
    lv_obj_set_size(root, 320, 480);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
}

static lv_obj_t *ui_make_status(lv_obj_t *root, const char *text, lv_obj_t **out_label)
{
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, 320, UI_STATUS_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(bar);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &s_style_status, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    if (out_label != NULL) {
        *out_label = label;
    }
    return bar;
}

static lv_obj_t *ui_make_content(lv_obj_t *root)
{
    lv_obj_t *content = lv_obj_create(root);
    lv_obj_set_size(content, 320, UI_CONTENT_H);
    lv_obj_set_pos(content, 0, UI_CONTENT_Y);
    lv_obj_set_style_bg_color(content, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, UI_PAD, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    return content;
}

static lv_obj_t *ui_make_hint(lv_obj_t *root, const char *text)
{
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, 320, UI_HINT_H);
    lv_obj_set_pos(bar, 0, 480 - UI_HINT_H);
    lv_obj_set_style_bg_color(bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(bar);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &s_style_hint, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    return bar;
}

static lv_obj_t *ui_make_label(lv_obj_t *parent, const char *text, bool title)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, title ? &s_style_title : &s_style_body, 0);
    lv_obj_set_width(label, 280);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *ui_make_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 280, 96);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &s_style_card, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/* ===================== 公开接口：给别的 UI 模块建标签 ===================== */

/**
 * 建一个正文 / 标题标签（样式与页面内一致）。
 *
 * 供 src/ui/ui_ota_page.cpp 用 —— 它要自己持有这个标签并定时刷新它的文字。
 */
lv_obj_t *ui_pages_make_label(lv_obj_t *parent, const char *text, bool title)
{
    return ui_make_label(parent, text, title);
}

/* 列表项: 文案 + 右侧副文案, 返回可点击的整行对象 */
static lv_obj_t *ui_make_list_item_ex(lv_obj_t *parent, const char *text, const char *sub,
                                      lv_obj_t **out_sub)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 280, 34);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_add_style(label, &s_style_body, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    /* 登记为可选行（光标用）：行首的 "> " / "  " 标记在渲染时按光标位置重写 */
    if (s_build_page >= 0 && s_build_page < (int)PAGE_COUNT) {
        uint8_t n = s_item_count[s_build_page];
        if (n < UI_MAX_ITEMS) {
            const char *t = text;
            while (*t == ' ') {
                t++;
            }
            if (*t == '>') {
                t++;
                while (*t == ' ') {
                    t++;
                }
            }
            snprintf(s_items[s_build_page][n].text, UI_ITEM_TEXT_MAX, "%s", t);
            s_items[s_build_page][n].row = row;
            s_items[s_build_page][n].label = label;
            s_item_count[s_build_page] = n + 1;
        }
    }

    if (sub != NULL) {
        lv_obj_t *right = lv_label_create(row);
        lv_label_set_text(right, sub);
        lv_obj_add_style(right, &s_style_hint, 0);
        lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
        if (out_sub != NULL) {
            *out_sub = right;
        }
    }
    return row;
}

static lv_obj_t *ui_make_list_item(lv_obj_t *parent, const char *text, const char *sub)
{
    return ui_make_list_item_ex(parent, text, sub, NULL);
}

/* ===================== 各页内容 ===================== */

/* 0. 首次开机引导 (PAGE_OOBE)
 *
 * 首屏 = **品牌区**（设计稿整屏底图，见 src/ui/wifi_bg.c）+ 下方两个选项：
 *   - 「连接 Wi-Fi 网络」→ PAGE_WIFI_SCAN
 *
 * 品牌区按设计稿原样呈现（不重画标题、不改文案），这一层只负责交互。
 * 接入进度由 `ui_pages_set_pair_state()` 刷新，UI 不自己计时、不发网络请求。
 */

/* 首屏品牌区底图：设计稿整屏 320x480，按 LVGL 原生 16 位色内存布局存放。
 * LVGL 的 lv_img_dsc_t 要求 header.cf = LV_IMG_CF_TRUE_COLOR、data_size = w*h*2。 */
static lv_img_dsc_t s_oobe_bg_dsc;

static void ui_oobe_bg_init(void)
{
    s_oobe_bg_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_oobe_bg_dsc.header.w = UI_WIFI_BG_WIDTH;
    s_oobe_bg_dsc.header.h = UI_WIFI_BG_HEIGHT;
    s_oobe_bg_dsc.data_size = UI_WIFI_BG_PIXELS * 2;
    /* 数组是 const uint16_t；这里按字节视图交给 LVGL（两种 const 声明都能编译）。 */
    s_oobe_bg_dsc.data = (uint8_t *)(uintptr_t)ui_wifi_bg;
}

static void ui_build_oobe(lv_obj_t *content)
{
    lv_obj_t *root = lv_obj_get_parent(content);

    /* 品牌区 = 设计稿整屏底图 (320x480), 作为 img 部件铺满整页并压到最底层。
     * 走 lv_img_set_src + 变量图源 (LV_IMG_CF_TRUE_COLOR) ——
     * LVGL 会按屏幕自身的 lv_color_t 布局读像素, 颜色与 UI 其它元素一致。 */
    lv_obj_t *bg = lv_img_create(root);
    lv_img_set_src(bg, &s_oobe_bg_dsc);
    lv_obj_set_pos(bg, 0, 0);
    lv_obj_move_to_index(bg, 0);

    /* 状态条 / 内容层 / 提示条都让位给品牌区 (透明, 底图透出来)。 */
    lv_obj_set_style_bg_opa(s_pages[PAGE_OOBE].status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_pages[PAGE_OOBE].hint, LV_OPA_TRANSP, 0);

    /* 状态行：等待连接网络 → 连接中… → 已联网
     * （字库里没有 ● / ✓，用字库已有的 · 代替） */
    s_oobe_state_label = ui_make_label(content, "等待连接网络", false);
    lv_obj_set_pos(s_oobe_state_label, 8, 268);

    lv_obj_t *item1 = ui_make_list_item(content, "> [ 连接 Wi-Fi 网络 ]", NULL);
    lv_obj_set_pos(item1, 8, 304);
}

/** 按接入状态刷新 OOBE 状态行（文案温和，不出现任何技术术语）。 */
void ui_pages_set_pair_state(int state)
{
    if (s_oobe_state_label == NULL) {
        return;
    }
    const char *text;
    switch (state) {
    case UI_PAIR_STATE_PENDING:
        text = "· 连接中…";
        break;
    case UI_PAIR_STATE_READY:
        text = "· 已联网";
        break;
    case UI_PAIR_STATE_FAILED:
        text = "· 暂时没连上，会自动再试一次";
        break;
    default:
        text = "等待连接网络";
        break;
    }
    lv_label_set_text(s_oobe_state_label, text);
}

/* 1. 主仪表盘 (PAGE_HOME)
 *
 * 天气行是**真实数据**：由 src/ui/ui_router.cpp 在收到服务端心跳应答后写入
 * （doc/05 §5.2 的 weather{temp,text}）。拿不到时显示温和提示，不显示假数据。
 */
static void ui_build_home(lv_obj_t *content)
{
    lv_obj_t *card = ui_make_card(content);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *label = ui_make_label(card, "今日课程\n暂时读不到课表", false);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_home_schedule_label = label;

    /* 天气：与服务端心跳应答同步；初始为空态（还没拿到数据） */
    lv_obj_t *weather = ui_make_label(card, "暂时拿不到天气", false);
    lv_obj_align(weather, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_home_weather_label = weather;

    static const char *items[] = {
        "> [ 拍题导学 ]      剩余 8 / 共 10",
        "  [ 语音提问 ]      说出你的想法",
        "  [ 学科卡片 ]      已收录 4 个包",
        "  [ 电子书架 ]      今日剩 45 分钟",
        "  [ 专注番茄 ]      25分钟倒计时",
        "  [ 系统设置 ]      WiFi/音量/OTA",
    };
    int i;
    for (i = 0; i < 6; i++) {
        lv_obj_t *row = ui_make_list_item(content, items[i], NULL);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 110 + i * 36);
    }
}

/* 2. 拍题取景 (PAGE_CAMERA) */
static void ui_build_camera(lv_obj_t *content)
{
    lv_obj_t *frame = lv_obj_create(content);
    lv_obj_set_size(frame, 280, 300);
    lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_border_color(frame, UI_C_ACCENT, 0);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(frame, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *tip = ui_make_label(content, "把题目放进取景框，保持平稳", false);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* 3. 解析结果 (PAGE_SOLVE_RESULT) */
static void ui_build_solve_result(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "解题步骤", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = ui_make_label(content,
        "1. 先圈出已知条件\n2. 找出它们之间的联系\n3. 一步一步往下推\n4. 回头检查每一步", false);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 36);
    s_solve_body = body;

    lv_obj_t *q = ui_make_label(content, "还有哪里没想明白？", false);
    lv_obj_align(q, LV_ALIGN_BOTTOM_LEFT, 0, -34);
    s_solve_question = q;

    lv_obj_t *menu = ui_make_list_item(content, "> [ 口述你的思路 ]", NULL);
    lv_obj_align(menu, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

/* 4. 学科卡片包列表 (PAGE_SUBJECT_LIST)
 *
 * 内容由 `ui_pages_refresh_subject_list()` 从 TF 卡的 /subjects 重建
 * （这里只放标题占位）—— 列表显示的是**真实已下载**的学科包，不是写死的假数据。
 */
static void ui_build_subject_list(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "学科包", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
}

/* 5. 翻卡学习 (PAGE_CARD_STUDY)
 *
 * 正面是题干，短按翻面看答案，长按退出（doc/03 §四 手势表）。
 * 左右键在卡片之间翻。
 */
static void ui_build_card_study(lv_obj_t *content)
{
    lv_obj_t *card = ui_make_card(content);
    lv_obj_set_size(card, 280, 260);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *label = ui_make_label(card, "还没有打开学科包", false);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    s_card_label = label;

    lv_obj_t *pos = ui_make_label(content, "", false);
    lv_obj_align(pos, LV_ALIGN_TOP_RIGHT, 0, 6);
    s_card_pos_label = pos;

    /* 菜单只有两项是**可确认**的（doc/03 §四）：翻面 / 语音追问。
     * 第三行是操作提示（左右键翻卡），用普通标签 —— 不是可选行。 */
    static const char *items[] = {
        "> [ 翻面 ]",
        "  [ 语音追问这题 ]",
    };
    int i;
    for (i = 0; i < 2; i++) {
        lv_obj_t *row = ui_make_list_item(content, items[i], NULL);
        lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -70 + i * 26);
    }
    lv_obj_t *hint = ui_make_label(content, "  左右键换一张", false);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, -18);
}

/* 6. 番茄专注时钟 (PAGE_POMODORO) */
static void ui_build_pomodoro(lv_obj_t *content)
{
    lv_obj_t *time = ui_make_label(content, "25:00", true);
    lv_obj_align(time, LV_ALIGN_CENTER, 0, -20);
    s_pomo_time_label = time;

    lv_obj_t *sub = ui_make_label(content, "准备开始", false);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 20);
    s_pomo_sub_label = sub;

    lv_obj_t *note = ui_make_label(content, "到点会响一声提示音", false);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* 8. 系统设置 (PAGE_SETTINGS)
 *
 * 「所在城市」是真入口：短按进入候选城市列表，选中后落盘 TF 卡并随心跳上报
 * （doc/05 §5.2）。右侧显示当前城市名（还没设置过时显示「未设置」）。
 */
static void ui_build_settings(lv_obj_t *content)
{
    static const char *items[] = {
        "> 无线网络",
        "  音量",
        "  屏幕亮度",
        "  固件升级",
        "  电池与省电",
        "  关于设备",
    };
    int i;
    for (i = 0; i < 6; i++) {
        lv_obj_t *row = ui_make_list_item(content, items[i], NULL);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, i * 40);
    }
    /* 第 7 项：专注时长（右侧显示当前分钟数） */
    lv_obj_t *focus_row = ui_make_list_item_ex(content, "  专注时长", "", &s_focus_value_label);
    lv_obj_align(focus_row, LV_ALIGN_TOP_LEFT, 0, 6 * 40);
    if (s_focus_value_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d 分钟", s_focus_minutes);
        lv_label_set_text(s_focus_value_label, buf);
    }
    /* 第 8 项：所在城市（右侧显示当前城市；短按进列表选） */
    lv_obj_t *city_row = ui_make_list_item_ex(content, "  所在城市", "", &s_city_value_label);
    lv_obj_align(city_row, LV_ALIGN_TOP_LEFT, 0, 7 * 40);
    if (s_city_value_label != NULL) {
        lv_label_set_text(s_city_value_label, "未设置");
    }
    /* 第 9 项：离线手动校时（doc/04 §1.3 时钟熔断时的人工兜底） */
    lv_obj_t *clock_row = ui_make_list_item(content, "  手动校时", NULL);
    lv_obj_align(clock_row, LV_ALIGN_TOP_LEFT, 0, 8 * 40);
    /* 第 10 项：关机（doc/02 §3.6 关机契约）
     * 短按进待确认态（文案变「关机（长按确认）」），再长按摇杆确认关机。 */
    lv_obj_t *shutdown_row = ui_make_list_item(content, "  关机", NULL);
    lv_obj_align(shutdown_row, LV_ALIGN_TOP_LEFT, 0, 9 * 40);
}

/* ===================== 13. 选择城市 (PAGE_CITY)
 *
 * 候选城市列表（虚拟键盘未落地时的入口）：上下移动光标，短按确认。
 * 确认后：落盘 TF 卡（离线兜底）+ 写进 net 层缓存（随下一次心跳上报）。
 * ===================================================== */
#define CITY_VISIBLE_ROWS 8   /* 一屏能显示几行 */

/* 「不登记进通用光标」的可选行（定义在下面的 Wi-Fi 段里，这里先前置声明） */
static lv_obj_t *ui_make_row(lv_obj_t *parent, lv_obj_t **out_label);

static lv_obj_t *s_city_rows[CITY_VISIBLE_ROWS];
static lv_obj_t *s_city_labels[CITY_VISIBLE_ROWS];
static int s_city_cursor = 0;    /* 选中的候选序号 */
static int s_city_scroll = 0;     /* 列表滚动偏移 */

/** 重画候选城市列表（带光标标记）。 */
static void ui_city_render(void)
{
    int total = ui_weather_city_candidate_count();
    int i;
    for (i = 0; i < CITY_VISIBLE_ROWS; i++) {
        int idx = s_city_scroll + i;
        if (s_city_labels[i] == NULL) {
            continue;
        }
        if (idx >= total) {
            lv_obj_add_flag(s_city_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_city_rows[i], LV_OBJ_FLAG_HIDDEN);
        const ui_city_candidate_t *c = ui_weather_city_candidate(idx);
        char buf[40];
        snprintf(buf, sizeof(buf), "%s%s", (idx == s_city_cursor) ? "> " : "  ",
                 (c != NULL) ? c->zh : "");
        lv_label_set_text(s_city_labels[i], buf);
    }
}

void ui_pages_city_enter(void)
{
    s_city_cursor = 0;
    s_city_scroll = 0;
    ui_city_render();
}

int ui_pages_city_move(int delta)
{
    int total = ui_weather_city_candidate_count();
    if (total <= 0) {
        return -1;
    }
    int c = s_city_cursor + delta;
    if (c < 0) {
        c = 0;
    }
    if (c >= total) {
        c = total - 1;
    }
    s_city_cursor = c;
    /* 滚动：让光标始终在可见窗口里 */
    if (c < s_city_scroll) {
        s_city_scroll = c;
    } else if (c >= s_city_scroll + CITY_VISIBLE_ROWS) {
        s_city_scroll = c - CITY_VISIBLE_ROWS + 1;
    }
    ui_city_render();
    return s_city_cursor;
}

const ui_city_candidate_t *ui_pages_city_selected(void)
{
    return ui_weather_city_candidate(s_city_cursor);
}

static void ui_build_city(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "选择所在城市", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    int i;
    for (i = 0; i < CITY_VISIBLE_ROWS; i++) {
        lv_obj_t *row = ui_make_row(content, &s_city_labels[i]);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 44 + i * 36);
        s_city_rows[i] = row;
    }
    ui_city_render();
}

/* ===================== 9. Wi-Fi 扫描列表 (PAGE_WIFI_SCAN)
 *
 * 页面上显示的是**真扫描结果**：进页时向网络任务提交扫描请求，
 * 心跳里轮询缓存回来的结果，按信号强弱排序、同名合并，标出加密与已保存。
 * 底层扫描与连接都在 Core 0 的网络任务里做（doc/02 §4.1），UI 不阻塞。
 * ===================================================== */

#define WIFI_VISIBLE_ROWS 8   /* 屏幕里能同时显示的热点行数 */
#define WIFI_ROW_H        34  /* 行高（与别页的行高一致） */
#define WIFI_RAW_MAX      16  /* 一次扫描最多缓存的热点数 */

/* 静态缓冲：UI 核栈只有 4KB，列表构建路径不开大缓冲 */
static struct WifiApInfo s_wifi_raw[WIFI_RAW_MAX];
static struct WifiApInfo s_wifi_aps[WIFI_MODEL_MAX_APS];
static lv_obj_t *s_wifi_status_label = NULL;
static lv_obj_t *s_wifi_row_label[WIFI_VISIBLE_ROWS];
static lv_obj_t *s_wifi_rescan_label = NULL;
static int s_wifi_ap_count = 0;
static int s_wifi_cursor = 0;     /* 0..ap_count-1 = 热点；= ap_count 时是「重新扫描」 */
static int s_wifi_scroll = 0;      /* 列表滚动偏移 */
static wifi_conn_state_t s_wifi_conn;
static uint32_t s_wifi_result_seq = 0;     /* 已看过的连接结果序号 */
static uint32_t s_wifi_seen_scan_seq = 0;   /* 已看过的扫描结果序号 */
static char s_wifi_line[UI_ITEM_TEXT_MAX + 4];
static char s_wifi_pending_ssid[33];    /* 正在连接的目标 */
static char s_wifi_connected_ssid[33];  /* 已经连上的那个 */

/** 建一行「不登记进通用光标」的可选行（Wi-Fi 页与键盘页各自管光标）。 */
static lv_obj_t *ui_make_row(lv_obj_t *parent, lv_obj_t **out_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, WIFI_ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "");
    lv_obj_add_style(label, &s_style_body, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    if (out_label != NULL) {
        *out_label = label;
    }
    return row;
}

static void ui_build_wifi_scan(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "附近的无线网络", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    s_wifi_status_label = ui_make_label(content, "正在搜索附近的无线网络…", false);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 26);

    int i;
    for (i = 0; i < WIFI_VISIBLE_ROWS; i++) {
        lv_obj_t *row = ui_make_row(content, &s_wifi_row_label[i]);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 58 + i * WIFI_ROW_H);
    }

    /* 最后一行固定是「重新扫描」 */
    lv_obj_t *rescan = ui_make_row(content, &s_wifi_rescan_label);
    lv_obj_align(rescan, LV_ALIGN_TOP_LEFT, 0, 58 + WIFI_VISIBLE_ROWS * WIFI_ROW_H);
    lv_label_set_text(s_wifi_rescan_label, "  [ 重新扫描周边热点 ]");
}

/** 重画 Wi-Fi 列表（真数据 + 光标标记）。 */
static void ui_wifi_render_rows(void)
{
    char buf[UI_ITEM_TEXT_MAX];
    int i;
    for (i = 0; i < WIFI_VISIBLE_ROWS; i++) {
        int idx = s_wifi_scroll + i;
        if (idx >= s_wifi_ap_count) {
            lv_label_set_text(s_wifi_row_label[i], "");
            continue;
        }
        int connected = (s_wifi_connected_ssid[0] != '\0' &&
                         strcmp(s_wifi_aps[idx].ssid, s_wifi_connected_ssid) == 0);
        wifi_model_format_row(buf, sizeof(buf), &s_wifi_aps[idx], connected);
        snprintf(s_wifi_line, sizeof(s_wifi_line), "%s%s",
                 (idx == s_wifi_cursor) ? "> " : "  ", buf);
        lv_label_set_text(s_wifi_row_label[i], s_wifi_line);
        /* 选中态：光标所在行高亮，其余保持常规文字色 */
        lv_obj_set_style_text_color(s_wifi_row_label[i],
                                    (idx == s_wifi_cursor) ? UI_C_ACCENT : UI_C_TEXT, 0);
    }
    snprintf(s_wifi_line, sizeof(s_wifi_line), "%s[ 重新扫描周边热点 ]",
             (s_wifi_cursor == s_wifi_ap_count) ? "> " : "  ");
    lv_label_set_text(s_wifi_rescan_label, s_wifi_line);
}

/** 重画顶部状态行（连接中 / 成功 / 失败 / 没扫到）。 */
static void ui_wifi_render_status(void)
{
    if (s_wifi_status_label == NULL) {
        return;
    }
    const char *text = wifi_conn_hint(&s_wifi_conn);
    if (s_wifi_conn.phase == WIFI_CONN_SCANNING) {
        text = "正在搜索附近的无线网络…";
    } else if (s_wifi_conn.phase == WIFI_CONN_IDLE && s_wifi_ap_count == 0) {
        text = "附近没搜到无线网络，靠近路由器再试一次";
    } else if (s_wifi_conn.phase == WIFI_CONN_FAILED) {
        text = "无线网络连接失败，请核对密码后重试";
    }
    lv_label_set_text(s_wifi_status_label, text);
}

/** 把网络任务缓存回来的扫描结果整理成列表。 */
static void ui_wifi_take_scan_results(void)
{
    int n = net_wifi_get_cached_scan(s_wifi_raw, WIFI_RAW_MAX);
    if (n <= 0) {
        s_wifi_ap_count = 0;
        wifi_conn_scan_finish(&s_wifi_conn, 0);
        ui_wifi_render_rows();
        ui_wifi_render_status();
        return;
    }
    /* 排序 + 同名合并 + 标出已保存的网络 */
    n = wifi_model_sort_aps(s_wifi_raw, n);
    char saved[33];
    if (!net_wifi_get_saved_ssid(saved, sizeof(saved))) {
        saved[0] = '\0';
    }
    wifi_model_mark_saved(s_wifi_raw, n, saved);
    if (n > WIFI_MODEL_MAX_APS) {
        n = WIFI_MODEL_MAX_APS;
    }
    memcpy(s_wifi_aps, s_wifi_raw, (size_t)n * sizeof(struct WifiApInfo));
    s_wifi_ap_count = n;
    s_wifi_cursor = 0;
    s_wifi_scroll = 0;
    wifi_conn_scan_finish(&s_wifi_conn, n);
    ui_wifi_render_rows();
    ui_wifi_render_status();
}

void ui_pages_wifi_scan_enter(uint32_t now_ms)
{
    /* 从键盘输完密码刚回来：连接已经在进行中，别把状态冲掉（否则会白扫一次） */
    if (s_wifi_conn.phase == WIFI_CONN_CONNECTING) {
        ui_wifi_render_rows();
        ui_wifi_render_status();
        return;
    }
    wifi_conn_state_init(&s_wifi_conn);
    s_wifi_ap_count = 0;
    s_wifi_cursor = 0;
    s_wifi_scroll = 0;
    s_wifi_result_seq = net_wifi_connect_result_seq();
    net_wifi_request_scan();          /* UI 只提交请求 */
    wifi_conn_scan_start(&s_wifi_conn, now_ms);
    ui_wifi_render_rows();
    ui_wifi_render_status();
}

void ui_pages_wifi_scan_tick(uint32_t now_ms)
{
    /* 1) 扫描结果到了没 */
    if (s_wifi_conn.phase == WIFI_CONN_SCANNING) {
        uint32_t seq = net_wifi_scan_seq();
        if (seq != s_wifi_seen_scan_seq && net_wifi_scan_cache_ready()) {
            s_wifi_seen_scan_seq = seq;
            ui_wifi_take_scan_results();
        } else if (wifi_conn_scan_expired(&s_wifi_conn, now_ms)) {
            /* 扫太久没结果：给温和提示，不空屏 */
            wifi_conn_scan_finish(&s_wifi_conn, 0);
            ui_wifi_render_status();
        }
    }

    /* 2) 连接结果 */
    if (s_wifi_conn.phase == WIFI_CONN_CONNECTING) {
        if (net_wifi_connect_result_seq() != s_wifi_result_seq) {
            s_wifi_result_seq = net_wifi_connect_result_seq();
            bool ok = (net_wifi_connect_result() == 1);
            wifi_conn_connect_finish(&s_wifi_conn, ok);
            if (ok) {
                /* 连上了：记住是哪个，后面列表里给它标「已连」 */
                snprintf(s_wifi_connected_ssid, sizeof(s_wifi_connected_ssid), "%s",
                         s_wifi_pending_ssid);
                /* 再刷新列表把刚连的网络标成「已保存」 */
                char saved[33];
                if (net_wifi_get_saved_ssid(saved, sizeof(saved))) {
                    wifi_model_mark_saved(s_wifi_aps, s_wifi_ap_count, saved);
                    ui_wifi_render_rows();
                }
            }
            ui_wifi_render_status();
        } else if (wifi_conn_connect_expired(&s_wifi_conn, now_ms)) {
            wifi_conn_connect_finish(&s_wifi_conn, false);
            ui_wifi_render_status();
        }
    }
}

int ui_pages_wifi_scan_move(int delta)
{
    int last = s_wifi_ap_count;   /* 最后一项是「重新扫描」 */
    int c = s_wifi_cursor + delta;
    if (c < 0) {
        c = 0;
    }
    if (c > last) {
        c = last;
    }
    s_wifi_cursor = c;
    /* 滚动：让光标始终在可见窗口里 */
    if (c < s_wifi_scroll) {
        s_wifi_scroll = c;
    } else if (c >= s_wifi_scroll + WIFI_VISIBLE_ROWS) {
        s_wifi_scroll = c - WIFI_VISIBLE_ROWS + 1;
    }
    ui_wifi_render_rows();
    return c;
}

void ui_pages_wifi_scan_connect(const char *ssid, const char *password, uint32_t now_ms)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    wifi_conn_connect_start(&s_wifi_conn, ssid, now_ms);
    snprintf(s_wifi_pending_ssid, sizeof(s_wifi_pending_ssid), "%s", ssid);
    s_wifi_result_seq = net_wifi_connect_result_seq();
    net_wifi_submit_connect(ssid, password ? password : "");  /* 密码只进内存，不进日志 */
    ui_wifi_render_status();
}

void ui_pages_wifi_scan_action(uint32_t now_ms)
{
    if (s_wifi_conn.phase == WIFI_CONN_CONNECTING) {
        return;   /* 正在连，不重复发起 */
    }
    if (s_wifi_cursor >= s_wifi_ap_count) {
        /* 重新扫描 */
        s_wifi_ap_count = 0;
        s_wifi_cursor = 0;
        s_wifi_scroll = 0;
        net_wifi_request_scan();
        wifi_conn_scan_start(&s_wifi_conn, now_ms);
        ui_wifi_render_rows();
        ui_wifi_render_status();
        return;
    }
    const struct WifiApInfo *ap = &s_wifi_aps[s_wifi_cursor];
    if (ap->is_encrypted) {
        /* 加密网络：先进键盘输密码 */
        ui_pages_keyboard_enter(ap->ssid, true, now_ms);
        ui_router_go(PAGE_KEYBOARD);
        return;
    }
    ui_pages_wifi_scan_connect(ap->ssid, "", now_ms);
}

/* ===================== 10. 虚拟键盘 (PAGE_KEYBOARD)
 *
 * 真键盘：能移动光标、输入、退格、空格、大小写切换、显/隐、清空、确认。
 * 编辑逻辑全在 keypad_editor.c（纯逻辑，主机侧有测试），
 * 这一层只负责画键位与把摇杆动作翻译成编辑动作。
 * ===================================================== */

#define KEY_ROWS      6
#define KEY_COLS      10
#define KEY_ROW_H     38
#define KEY_TOP_Y     74
#define KEY_IDLE_MS   60000UL   /* 60 秒无操作退出（doc/03 §五 第 12 页） */

typedef enum {
    K_CHAR = 0,  /* 普通字符 */
    K_SHIFT,     /* 大小写切换 */
    K_SPACE,     /* 空格 */
    K_BACK,      /* 退格 */
    K_LEFT,      /* 光标左移 */
    K_RIGHT,     /* 光标右移 */
    K_MASK,      /* 显 / 隐 */
    K_CLEAR,     /* 清除 */
    K_OK,        /* 确认 */
    K_LAYER,     /* 字母层 <-> 数字符号层 */
} keypad_key_act_t;

typedef struct {
    const char *label;        /* 功能键的显示文字（普通字符键为 NULL） */
    char ch;                  /* 普通字符键的字符（字母层为小写） */
    keypad_key_act_t act;
} keypad_key_t;

/* 字母层: 三排字母 + 三排功能键 */
static const keypad_key_t s_key_rows_alpha[KEY_ROWS][KEY_COLS] = {
    { { NULL, 'q', K_CHAR }, { NULL, 'w', K_CHAR }, { NULL, 'e', K_CHAR },
      { NULL, 'r', K_CHAR }, { NULL, 't', K_CHAR }, { NULL, 'y', K_CHAR },
      { NULL, 'u', K_CHAR }, { NULL, 'i', K_CHAR }, { NULL, 'o', K_CHAR },
      { NULL, 'p', K_CHAR } },
    { { NULL, 'a', K_CHAR }, { NULL, 's', K_CHAR }, { NULL, 'd', K_CHAR },
      { NULL, 'f', K_CHAR }, { NULL, 'g', K_CHAR }, { NULL, 'h', K_CHAR },
      { NULL, 'j', K_CHAR }, { NULL, 'k', K_CHAR }, { NULL, 'l', K_CHAR },
      { NULL, '-', K_CHAR } },
    { { NULL, 'z', K_CHAR }, { NULL, 'x', K_CHAR }, { NULL, 'c', K_CHAR },
      { NULL, 'v', K_CHAR }, { NULL, 'b', K_CHAR }, { NULL, 'n', K_CHAR },
      { NULL, 'm', K_CHAR }, { NULL, ',', K_CHAR }, { NULL, '.', K_CHAR },
      { NULL, '/', K_CHAR } },
    { { "123", 0, K_LAYER }, { "空格", 0, K_SPACE },
      { "大小写", 0, K_SHIFT }, { "退格", 0, K_BACK } },
    { { "左移", 0, K_LEFT }, { "右移", 0, K_RIGHT },
      { "显/隐", 0, K_MASK }, { "清除", 0, K_CLEAR } },
    { { "确认并连接", 0, K_OK } },
};

/* 数字符号层: 数字 + 常用符号（密码里要用） */
static const keypad_key_t s_key_rows_sym[KEY_ROWS][KEY_COLS] = {
    { { NULL, '1', K_CHAR }, { NULL, '2', K_CHAR }, { NULL, '3', K_CHAR },
      { NULL, '4', K_CHAR }, { NULL, '5', K_CHAR }, { NULL, '6', K_CHAR },
      { NULL, '7', K_CHAR }, { NULL, '8', K_CHAR }, { NULL, '9', K_CHAR },
      { NULL, '0', K_CHAR } },
    { { NULL, '!', K_CHAR }, { NULL, '@', K_CHAR }, { NULL, '#', K_CHAR },
      { NULL, '$', K_CHAR }, { NULL, '%', K_CHAR }, { NULL, '^', K_CHAR },
      { NULL, '&', K_CHAR }, { NULL, '*', K_CHAR }, { NULL, '(', K_CHAR },
      { NULL, ')', K_CHAR } },
    { { NULL, '-', K_CHAR }, { NULL, '_', K_CHAR }, { NULL, '+', K_CHAR },
      { NULL, '=', K_CHAR }, { NULL, '[', K_CHAR }, { NULL, ']', K_CHAR },
      { NULL, '{', K_CHAR }, { NULL, '}', K_CHAR }, { NULL, ';', K_CHAR },
      { NULL, ':', K_CHAR } },
    { { "ABC", 0, K_LAYER }, { "空格", 0, K_SPACE },
      { "大小写", 0, K_SHIFT }, { "退格", 0, K_BACK } },
    { { "左移", 0, K_LEFT }, { "右移", 0, K_RIGHT },
      { "显/隐", 0, K_MASK }, { "清除", 0, K_CLEAR } },
    { { "确认并连接", 0, K_OK } },
};

/* 每行的键数与格宽（合计不超过 300px 内容宽度；两层几何一致，便于原地换层） */
static const uint8_t s_key_count[KEY_ROWS] = { 10, 10, 10, 4, 4, 1 };
static const uint16_t s_key_w[KEY_ROWS] = { 30, 30, 30, 75, 75, 300 };

static lv_obj_t *s_key_obj[KEY_ROWS][KEY_COLS];
static lv_obj_t *s_key_label[KEY_ROWS][KEY_COLS];
static lv_obj_t *s_key_title_label = NULL;
static lv_obj_t *s_key_input_label = NULL;
static keypad_editor_t s_keypad;
static int s_key_row = 0;
static int s_key_col = 0;
static int s_key_layer = 0;   /* 0 = 字母层, 1 = 数字符号层 */
static bool s_key_confirmed = false;
static uint32_t s_key_last_ms = 0;
static char s_key_input_line[KEYPAD_MAX_TEXT + 16];
static char s_key_title[64];
static char s_key_target[33];

static void ui_keyboard_render_input(void)
{
    char vis[KEYPAD_MAX_TEXT + 2];
    int n = keypad_editor_render(&s_keypad, vis, sizeof(vis));
    int c = s_keypad.cursor;
    if (c > n) {
        c = n;
    }
    /* 光标处插一个下划线 */
    memcpy(s_key_input_line, vis, (size_t)c);
    s_key_input_line[c] = '_';
    memcpy(s_key_input_line + c + 1, vis + c, (size_t)(n - c));
    s_key_input_line[n + 1] = '\0';
    if (s_key_input_label != NULL) {
        /* buf 要比「密码：」+ 输入行更长，否则 gcc 的 -Wformat-truncation 会报错 */
        char buf[KEYPAD_MAX_TEXT + 48];
        snprintf(buf, sizeof(buf), "密码：%s", s_key_input_line);
        lv_label_set_text(s_key_input_label, buf);
    }
}

static void ui_keyboard_render_keys(void)
{
    const keypad_key_t (*rows)[KEY_COLS] = (s_key_layer == 0)
        ? s_key_rows_alpha : s_key_rows_sym;
    int r, c;
    for (r = 0; r < KEY_ROWS; r++) {
        for (c = 0; c < s_key_count[r]; c++) {
            if (s_key_label[r][c] == NULL) {
                continue;
            }
            const keypad_key_t *k = &rows[r][c];
            char buf[32];
            if (k->act == K_CHAR) {
                buf[0] = keypad_editor_key_char(&s_keypad, k->ch);
                buf[1] = '\0';
            } else if (k->act == K_LAYER) {
                /* 换层键的标签指向「切过去的那一层」 */
                snprintf(buf, sizeof(buf), "%s", (s_key_layer == 0) ? "123" : "ABC");
            } else if (k->act == K_SHIFT) {
                snprintf(buf, sizeof(buf), "%s", s_keypad.shift ? "大写" : "大小写");
            } else {
                snprintf(buf, sizeof(buf), "%s", k->label);
            }
            lv_label_set_text(s_key_label[r][c], buf);
            bool sel = (r == s_key_row && c == s_key_col);
            lv_obj_set_style_text_color(s_key_label[r][c], sel ? UI_C_ACCENT : UI_C_TEXT, 0);
            lv_obj_set_style_bg_color(s_key_obj[r][c], sel ? UI_C_CARD : UI_C_BG, 0);
            lv_obj_set_style_bg_opa(s_key_obj[r][c], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

static void ui_build_keyboard(lv_obj_t *content)
{
    s_key_title_label = ui_make_label(content, "输入密码", true);
    lv_obj_align(s_key_title_label, LV_ALIGN_TOP_MID, 0, 0);

    s_key_input_label = ui_make_label(content, "密码：", false);
    lv_obj_align(s_key_input_label, LV_ALIGN_TOP_LEFT, 0, 28);

    int r, c;
    for (r = 0; r < KEY_ROWS; r++) {
        for (c = 0; c < s_key_count[r]; c++) {
            lv_obj_t *obj = lv_obj_create(content);
            lv_obj_set_size(obj, s_key_w[r] - 2, 32);
            lv_obj_set_pos(obj, c * s_key_w[r], KEY_TOP_Y + r * KEY_ROW_H);
            lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(obj, 1, 0);
            lv_obj_set_style_border_color(obj, UI_C_LINE, 0);
            lv_obj_set_style_radius(obj, 4, 0);
            lv_obj_set_style_pad_all(obj, 0, 0);
            lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *label = lv_label_create(obj);
            lv_label_set_text(label, "");
            lv_obj_add_style(label, &s_style_body, 0);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
            s_key_obj[r][c] = obj;
            s_key_label[r][c] = label;
        }
    }
}

void ui_pages_keyboard_enter(const char *title, bool masked, uint32_t now_ms)
{
    keypad_editor_init(&s_keypad);
    keypad_editor_set_mask(&s_keypad, masked);
    s_key_row = 0;
    s_key_col = 0;
    s_key_layer = 0;   /* 默认字母层 */
    s_key_confirmed = false;
    s_key_last_ms = now_ms;
    snprintf(s_key_target, sizeof(s_key_target), "%s",
             (title != NULL) ? title : "");
    snprintf(s_key_title, sizeof(s_key_title), "输入密码：%s",
             (s_key_target[0] != '\0') ? s_key_target : "网络");
    if (s_key_title_label != NULL) {
        lv_label_set_text(s_key_title_label, s_key_title);
    }
    ui_keyboard_render_input();
    ui_keyboard_render_keys();
}

void ui_pages_keyboard_move(int dx, int dy, uint32_t now_ms)
{
    s_key_last_ms = now_ms;
    if (dy != 0) {
        int r = s_key_row + dy;
        if (r < 0) {
            r = 0;
        }
        if (r >= KEY_ROWS) {
            r = KEY_ROWS - 1;
        }
        s_key_row = r;
        if (s_key_col >= s_key_count[s_key_row]) {
            s_key_col = s_key_count[s_key_row] - 1;
        }
    }
    if (dx != 0) {
        int c = s_key_col + dx;
        if (c < 0) {
            c = 0;
        }
        if (c >= s_key_count[s_key_row]) {
            c = s_key_count[s_key_row] - 1;
        }
        s_key_col = c;
    }
    ui_keyboard_render_keys();
}

void ui_pages_keyboard_action(uint32_t now_ms)
{
    s_key_last_ms = now_ms;
    const keypad_key_t (*rows)[KEY_COLS] = (s_key_layer == 0)
        ? s_key_rows_alpha : s_key_rows_sym;
    const keypad_key_t *k = &rows[s_key_row][s_key_col];
    switch (k->act) {
    case K_CHAR:
        if (s_key_layer == 0) {
            /* 字母层走编辑器的字符映射（大小写粘滞在这里收尾） */
            keypad_editor_type_key(&s_keypad, k->ch);
        } else {
            /* 数字符号层：符号原样写入，不受大小写影响 */
            keypad_editor_insert(&s_keypad, k->ch);
        }
        break;
    case K_LAYER:
        s_key_layer = (s_key_layer == 0) ? 1 : 0;
        keypad_editor_set_shift(&s_keypad, false);
        break;
    case K_SHIFT:
        keypad_editor_toggle_shift(&s_keypad);
        break;
    case K_SPACE:
        keypad_editor_space(&s_keypad);
        break;
    case K_BACK:
        keypad_editor_backspace(&s_keypad);
        break;
    case K_LEFT:
        keypad_editor_move_left(&s_keypad);
        break;
    case K_RIGHT:
        keypad_editor_move_right(&s_keypad);
        break;
    case K_MASK:
        keypad_editor_toggle_mask(&s_keypad);
        break;
    case K_CLEAR:
        keypad_editor_clear(&s_keypad);
        break;
    case K_OK:
        s_key_confirmed = true;
        break;
    default:
        break;
    }
    ui_keyboard_render_input();
    ui_keyboard_render_keys();
}

bool ui_pages_keyboard_confirmed(void)
{
    return s_key_confirmed;
}

const char *ui_pages_keyboard_text(void)
{
    return keypad_editor_text(&s_keypad);
}

const char *ui_pages_keyboard_target(void)
{
    return s_key_target;
}

bool ui_pages_keyboard_tick(uint32_t now_ms)
{
    if (s_key_last_ms == 0) {
        s_key_last_ms = now_ms;
        return false;
    }
    return (uint32_t)(now_ms - s_key_last_ms) > KEY_IDLE_MS;
}
void ui_pages_keyboard_leave(void)
{
    /* 清掉内存里的明文，别留在 buffer 里 */
    keypad_editor_clear(&s_keypad);
    s_key_confirmed = false;
}

/* 11. 固件升级 (PAGE_OTA)
 *
 * 正文（当前版本 / 最新版本 / 更新说明 / 下载进度 / 结果提示）是**真实数据**：
 * 由 src/ui/ui_ota_page.cpp 持有并定时刷新，数据源是网络任务写的 OTA 状态。
 */
static void ui_build_ota(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "固件升级", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    ui_ota_page_build(content);

    lv_obj_t *item = ui_make_list_item(content, "> [ 检查更新 ]", NULL);
    lv_obj_align(item, LV_ALIGN_BOTTOM_MID, 0, 0);
}

/* ===================== 学科卡片：公开接口 ===================== */

/** 刷新学科包列表（读卡上 /subjects 下的真实学科包目录）。 */
void ui_pages_refresh_subject_list(void)
{
    lv_obj_t *content = ui_page_content(PAGE_SUBJECT_LIST);
    if (content == NULL) {
        return;
    }
    lv_obj_clean(content);
    s_pack_count = 0;
    s_item_count[PAGE_SUBJECT_LIST] = 0;
    s_cursor[PAGE_SUBJECT_LIST] = 0;

    lv_obj_t *title = ui_make_label(content, "学科包", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    ui_data_file_t files[UI_DATA_MAX_FILES];
    uint32_t count = 0;
    int rc = ui_data_list_files(UI_DATA_DIR_SUBJECTS, files, UI_DATA_MAX_FILES, &count);
    if (rc != UI_DATA_OK) {
        lv_obj_t *empty = ui_make_label(content, "暂时读不到学科包", false);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    uint32_t i;
    /* 构建期登记可选行 —— 这一页是**运行时重建**的，
       所以要自己把 s_build_page 置上（否则光标找不到行）。 */
    s_build_page = PAGE_SUBJECT_LIST;
    for (i = 0; i < count && s_pack_count < UI_MAX_PACKS; i++) {
        const char *name = files[i].name;
        if (name[0] == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        size_t n = strlen(name);
        if (n >= UI_PACK_ID_MAX) {
            continue;
        }
        snprintf(s_pack_ids[s_pack_count], UI_PACK_ID_MAX, "%s", name);
        char row_text[UI_PACK_ID_MAX + 4];
        snprintf(row_text, sizeof(row_text), "  %s", name);
        lv_obj_t *row = ui_make_list_item(content, row_text, "已下载");
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 44 + s_pack_count * 40);
        s_pack_count++;
    }
    s_build_page = -1;
    if (s_pack_count == 0) {
        lv_obj_t *empty = ui_make_label(content, "还没有下载学科包", false);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    ui_cursor_render(PAGE_SUBJECT_LIST);
}

/** 当前选中的学科包 id（没有则空串）。 */
const char *ui_pages_selected_pack_id(void)
{
    int idx = ui_pages_cursor_index(PAGE_SUBJECT_LIST);
    if (idx < 0 || idx >= s_pack_count) {
        return "";
    }
    return s_pack_ids[idx];
}

/** 载入卡片集（UI 持有指针，不拷贝 —— 上层持有所有权）。 */
void ui_pages_set_cards(const card_pack_t *pack)
{
    s_cards = pack;
    s_card_index = 0;
    s_card_show_answer = false;
    ui_pages_show_card(0, false);
}

int ui_pages_card_count(void)
{
    return (s_cards != NULL) ? s_cards->count : 0;
}

int ui_pages_card_index(void)
{
    return s_card_index;
}

bool ui_pages_card_answer_shown(void)
{
    return s_card_show_answer;
}

const char *ui_pages_current_pack_id(void)
{
    return (s_cards != NULL) ? s_cards->pack_id : "";
}

const char *ui_pages_current_card_id(void)
{
    if (s_cards == NULL || s_card_index < 0 || s_card_index >= s_cards->count) {
        return "";
    }
    return s_cards->items[s_card_index].id;
}

/** 显示第 index 张卡（show_answer = true 显示背面）。 */
void ui_pages_show_card(int index, bool show_answer)
{
    if (s_cards == NULL || index < 0 || index >= s_cards->count) {
        return;
    }
    s_card_index = index;
    s_card_show_answer = show_answer;
    const card_item_t *item = &s_cards->items[index];
    if (s_card_label != NULL) {
        lv_label_set_text(s_card_label, show_answer ? item->answer : item->question);
    }
    if (s_card_pos_label != NULL) {
        char buf[48];
        snprintf(buf, sizeof(buf), "第 %d / %d 张", index + 1, s_cards->count);
        lv_label_set_text(s_card_pos_label, buf);
    }
}

/* ===================== 电子书子页面 (doc/07) ===================== */

static void ui_build_bookshelf(lv_obj_t *content)
{
    /* 书架内容由 ui_pages_refresh_bookshelf() 从 TF 卡重建（这里只放标题占位） */
    lv_obj_t *title = ui_make_label(content, "我的书架", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
}

static void ui_build_reader(lv_obj_t *content)
{
    lv_obj_t *body = ui_make_label(content, "还没有打开书籍", false);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 0);
    s_reader_body = body;
}

static void ui_build_book_menu(lv_obj_t *content)
{
    /* 内容由 ui_book_menu_render() 按真实状态重建（进页时清空重画）。 */
    lv_obj_t *title = ui_make_label(content, "阅 读 功 能 菜 单", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
}

static void ui_build_book_toc(lv_obj_t *content)
{
    /* 内容由 ui_book_toc_render() 按 .toc 真实章节重建。 */
    lv_obj_t *title = ui_make_label(content, "章节目录", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
}

static void ui_build_book_jump(lv_obj_t *content)
{
    /* 内容由 ui_book_jump_render() 按目标百分比重建。 */
    lv_obj_t *title = ui_make_label(content, "进度定位跳转", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
}

static void ui_build_book_locked(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "该休息啦", true);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -20);
    s_locked_title = title;

    lv_obj_t *body = ui_make_label(content, "现在是阅读休息时段\n明早再来读书吧", false);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 12);
    s_locked_body = body;
}

/* ===================== 14. 离线手动校时 (PAGE_MANUAL_CLOCK)
 *
 * doc/04 §1.3 时钟熔断（冷启动未校准 / 距上次校准超过 7 天）时，
 * 云端导学请求会被 net 层直接拒绝（net_clock_gate_user_request）。
 * 这一页是**人工兜底**：用户直接填一次本地墙钟（UTC+8），解掉熔断。
 *
 * 交互（doc/03 §3.2 六手势宪法，不自创手势）：
 *   上 / 下  = 在「日期」「时间」两组数字之间切换（当前组高亮 + 行首标记）
 *   左 / 右  = 数字键盘光标左右移动
 *   短按     = 输入当前键（数字 / 退格 / 确定）
 *   长按     = 退出回设置页
 *
 * 编辑状态复用 keypad_editor（纯逻辑，主机侧有单测）：两个编辑态各一个 ——
 * 日期 8 位（YYYYMMDD）、时间 4 位（HHMM）。光标恒在末尾，所以输入即追加、
 * 退格即删最后一位。
 *
 * 时间写入**只走 net 层 API**（net_clock_set_manual），UI 层不碰 hal_storage。
 * ===================================================== */

#define MC_KEY_COUNT   12
#define MC_KEY_W       26
#define MC_KEY_H       44
#define MC_KEY_TOP_Y   176
#define MC_DATE_LEN    8
#define MC_TIME_LEN    4

/* 数字键盘：0-9 + 退格 + 确定（单行一条，左右移动光标即可到达每一个键） */
static const char *s_mc_keys[MC_KEY_COUNT] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "退", "确"
};

static keypad_editor_t s_mc_date;   /* 日期：8 位 YYYYMMDD */
static keypad_editor_t s_mc_time;   /* 时间：4 位 HHMM */
static int s_mc_group = 0;          /* 0 = 日期组，1 = 时间组 */
static int s_mc_key = 0;            /* 数字键盘光标 0..MC_KEY_COUNT-1 */
static lv_obj_t *s_mc_date_label = NULL;
static lv_obj_t *s_mc_time_label = NULL;
static lv_obj_t *s_mc_key_obj[MC_KEY_COUNT];
static lv_obj_t *s_mc_key_label[MC_KEY_COUNT];

/** 把已输入的数字按 YYYY-MM-DD / HH:MM 排版（输入几位显示几位）。 */
static void mc_format_date(const char *digits, char *out, size_t cap)
{
    size_t n = strlen(digits);
    size_t o = 0;
    size_t i;
    for (i = 0; i < n && o + 1 < cap; i++) {
        if ((i == 4 || i == 6) && i < n) {
            out[o++] = '-';
        }
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

static void mc_format_time(const char *digits, char *out, size_t cap)
{
    size_t n = strlen(digits);
    size_t o = 0;
    size_t i;
    for (i = 0; i < n && o + 1 < cap; i++) {
        if (i == 2 && i < n) {
            out[o++] = ':';
        }
        out[o++] = digits[i];
    }
    out[o] = '\0';
}

/** 重画两行输入 + 键盘高亮。 */
static void mc_render(void)
{
    char buf[40];
    char shown[MC_DATE_LEN + 4];

    if (s_mc_date_label != NULL) {
        mc_format_date(keypad_editor_text(&s_mc_date), shown, sizeof(shown));
        snprintf(buf, sizeof(buf), "%s日期  %s", (s_mc_group == 0) ? "> " : "  ", shown);
        lv_label_set_text(s_mc_date_label, buf);
        lv_obj_set_style_text_color(s_mc_date_label,
                                    (s_mc_group == 0) ? UI_C_ACCENT : UI_C_TEXT, 0);
    }
    if (s_mc_time_label != NULL) {
        mc_format_time(keypad_editor_text(&s_mc_time), shown, sizeof(shown));
        snprintf(buf, sizeof(buf), "%s时间  %s", (s_mc_group == 1) ? "> " : "  ", shown);
        lv_label_set_text(s_mc_time_label, buf);
        lv_obj_set_style_text_color(s_mc_time_label,
                                    (s_mc_group == 1) ? UI_C_ACCENT : UI_C_TEXT, 0);
    }

    int i;
    for (i = 0; i < MC_KEY_COUNT; i++) {
        if (s_mc_key_label[i] == NULL) {
            continue;
        }
        bool sel = (i == s_mc_key);
        lv_obj_set_style_text_color(s_mc_key_label[i], sel ? UI_C_ACCENT : UI_C_TEXT, 0);
        lv_obj_set_style_bg_color(s_mc_key_obj[i], sel ? UI_C_CARD : UI_C_BG, 0);
        lv_obj_set_style_bg_opa(s_mc_key_obj[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void ui_build_manual_clock(lv_obj_t *content)
{
    lv_obj_t *title = ui_make_label(content, "离线手动校时", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *sub = ui_make_label(content, "北京时间（UTC+8）", false);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 28);

    s_mc_date_label = ui_make_label(content, "  日期", false);
    lv_obj_align(s_mc_date_label, LV_ALIGN_TOP_LEFT, 0, 70);

    s_mc_time_label = ui_make_label(content, "  时间", false);
    lv_obj_align(s_mc_time_label, LV_ALIGN_TOP_LEFT, 0, 104);

    int i;
    for (i = 0; i < MC_KEY_COUNT; i++) {
        lv_obj_t *obj = lv_obj_create(content);
        lv_obj_set_size(obj, MC_KEY_W - 2, MC_KEY_H);
        lv_obj_set_pos(obj, 2 + i * MC_KEY_W, MC_KEY_TOP_Y);
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, UI_C_LINE, 0);
        lv_obj_set_style_radius(obj, 4, 0);
        lv_obj_set_style_pad_all(obj, 0, 0);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(obj);
        lv_label_set_text(label, s_mc_keys[i]);
        lv_obj_add_style(label, &s_style_body, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        s_mc_key_obj[i] = obj;
        s_mc_key_label[i] = label;
    }
}

void ui_pages_manual_clock_enter(void)
{
    keypad_editor_init(&s_mc_date);
    keypad_editor_set_mask(&s_mc_date, false);
    keypad_editor_set_max_len(&s_mc_date, MC_DATE_LEN);
    keypad_editor_init(&s_mc_time);
    keypad_editor_set_mask(&s_mc_time, false);
    keypad_editor_set_max_len(&s_mc_time, MC_TIME_LEN);
    s_mc_group = 0;
    s_mc_key = 0;
    mc_render();
}

void ui_pages_manual_clock_move(int dx, int dy)
{
    if (dy != 0) {
        /* 上 = 日期组，下 = 时间组（两组数字间切换） */
        s_mc_group = (dy < 0) ? 0 : 1;
    }
    if (dx != 0) {
        int k = s_mc_key + dx;
        if (k < 0) {
            k = 0;
        }
        if (k >= MC_KEY_COUNT) {
            k = MC_KEY_COUNT - 1;
        }
        s_mc_key = k;
    }
    mc_render();
}

/** 解析数字串 → 各位数字。位数不对或含非数字即失败。 */
static bool mc_parse(const char *digits, int len, int *out)
{
    size_t i;
    if (strlen(digits) != (size_t)len) {
        return false;
    }
    for (i = 0; i < (size_t)len; i++) {
        if (digits[i] < '0' || digits[i] > '9') {
            return false;
        }
        out[i] = digits[i] - '0';
    }
    return true;
}

mc_result_t ui_pages_manual_clock_action(void)
{
    const char *key = s_mc_keys[s_mc_key];
    mc_result_t result = MC_RESULT_NONE;

    if (key[0] >= '0' && key[0] <= '9' && key[1] == '\0') {
        keypad_editor_t *ed = (s_mc_group == 0) ? &s_mc_date : &s_mc_time;
        keypad_editor_insert(ed, key[0]);
        /* 日期满 8 位自动进入时间组（两段式：先日期，后时间） */
        if (s_mc_group == 0 && s_mc_date.len >= MC_DATE_LEN) {
            s_mc_group = 1;
        }
    } else if (strcmp(key, "退") == 0) {
        keypad_editor_t *ed = (s_mc_group == 0) ? &s_mc_date : &s_mc_time;
        keypad_editor_backspace(ed);
    } else if (strcmp(key, "确") == 0) {
        int dv[MC_DATE_LEN];
        int tv[MC_TIME_LEN];
        if (!mc_parse(keypad_editor_text(&s_mc_date), MC_DATE_LEN, dv) ||
            !mc_parse(keypad_editor_text(&s_mc_time), MC_TIME_LEN, tv)) {
            result = MC_RESULT_BAD;
        } else {
            int year = dv[0] * 1000 + dv[1] * 100 + dv[2] * 10 + dv[3];
            int month = dv[4] * 10 + dv[5];
            int day = dv[6] * 10 + dv[7];
            int hour = tv[0] * 10 + tv[1];
            int minute = tv[2] * 10 + tv[3];
            /* 时间写入只走 net 层 API（UI 层不碰 hal_storage） */
            result = net_clock_set_manual(year, month, day, hour, minute)
                         ? MC_RESULT_OK : MC_RESULT_BAD;
        }
    }

    mc_render();
    return result;
}

/* ===================== 页面表 ===================== */

typedef void (*ui_page_builder_t)(lv_obj_t *content);

static const ui_page_builder_t s_page_builders[PAGE_COUNT] = {
    [PAGE_HOME]         = ui_build_home,
    [PAGE_OOBE]         = ui_build_oobe,
    [PAGE_WIFI_SCAN]    = ui_build_wifi_scan,
    [PAGE_SETTINGS]     = ui_build_settings,
    [PAGE_SUBJECT_LIST] = ui_build_subject_list,
    [PAGE_CARD_STUDY]   = ui_build_card_study,
    [PAGE_CAMERA]       = ui_build_camera,
    [PAGE_SOLVE_RESULT] = ui_build_solve_result,
    [PAGE_KEYBOARD]     = ui_build_keyboard,
    [PAGE_OTA]          = ui_build_ota,
    [PAGE_POMODORO]     = ui_build_pomodoro,
    [PAGE_CITY]         = ui_build_city,
    [PAGE_MANUAL_CLOCK] = ui_build_manual_clock,
};

static const ui_page_builder_t s_book_builders[PAGE_BOOK_COUNT - PAGE_BOOKSHELF] = {
    [PAGE_BOOKSHELF - PAGE_BOOKSHELF] = ui_build_bookshelf,
    [PAGE_READER - PAGE_BOOKSHELF]    = ui_build_reader,
    [PAGE_BOOK_MENU - PAGE_BOOKSHELF] = ui_build_book_menu,
    [PAGE_BOOK_TOC - PAGE_BOOKSHELF]  = ui_build_book_toc,
    [PAGE_BOOK_JUMP - PAGE_BOOKSHELF] = ui_build_book_jump,
    [PAGE_BOOK_LOCKED - PAGE_BOOKSHELF] = ui_build_book_locked,
};

/* 各页底部提示文案 (doc/03 §五) */
static const char *s_page_hints[PAGE_COUNT] = {
    [PAGE_HOME]         = "上下移动 | 短按确定 | 长按退出",
    [PAGE_OOBE]         = "上下选择 | 短按确定",
    [PAGE_WIFI_SCAN]    = "上下选择 | 短按连接 | 长按退出",
    [PAGE_SETTINGS]     = "上下选择 | 左右调时长 | 短按确定 | 长按退出",
    [PAGE_SUBJECT_LIST] = "上下选择 | 短按进入 | 长按退出",
    [PAGE_CARD_STUDY]   = "左右翻卡 | 短按确定 | 长按退出",
    [PAGE_CAMERA]       = "短按拍照 | 长按退出",
    [PAGE_SOLVE_RESULT] = "上下滚动 | 短按确定 | 长按退出",
    [PAGE_KEYBOARD]     = "上下选择 | 短按输入 | 长按退出",
    [PAGE_OTA]          = "上下选择 | 短按确定 | 长按退出",
    [PAGE_POMODORO]     = "短按暂停/继续 | 长按结束退出",
    [PAGE_CITY]         = "上下选择 | 短按确定 | 长按退出",
    [PAGE_MANUAL_CLOCK] = "上下切日期时间 | 左右选数字 | 短按输入 | 长按退出",
};

static const char *s_book_hints[PAGE_BOOK_COUNT - PAGE_BOOKSHELF] = {
    [PAGE_BOOKSHELF - PAGE_BOOKSHELF] = "上下选择 | 短按打开 | 长按退出",
    [PAGE_READER - PAGE_BOOKSHELF]    = "左右翻页 | 上下调亮度 | 长按退出",
    [PAGE_BOOK_MENU - PAGE_BOOKSHELF] = "上下选择 | 短按确定 | 长按退出",
    [PAGE_BOOK_TOC - PAGE_BOOKSHELF]  = "左右翻目录 | 短按跳转 | 长按退出",
    [PAGE_BOOK_JUMP - PAGE_BOOKSHELF] = "左右微调 | 长按退出",
    [PAGE_BOOK_LOCKED - PAGE_BOOKSHELF] = "长按退出",
};

/* ===================== 浮层 ===================== */

static void ui_build_overlays(lv_obj_t *parent)
{
    /* 提醒弹窗: 最高优先级, 居中卡片 */
    {
        lv_obj_t *root = lv_obj_create(parent);
        lv_obj_set_size(root, 320, 480);
        lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_50, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *card = lv_obj_create(root);
        lv_obj_set_size(card, 280, 160);
        lv_obj_remove_style_all(card);
        lv_obj_add_style(card, &s_style_card, LV_PART_MAIN);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *label = lv_label_create(card);
        lv_label_set_text(label, "提示");
        lv_obj_add_style(label, &s_style_title, 0);
        lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

        s_overlays[OVERLAY_REMINDER].root = root;
        s_overlays[OVERLAY_REMINDER].label = label;
    }

    /* 录音浮层: 中优先级, 顶部波形 + 倒计时 */
    {
        lv_obj_t *root = lv_obj_create(parent);
        lv_obj_set_size(root, 320, 120);
        lv_obj_set_pos(root, 0, 0);
        lv_obj_set_style_bg_color(root, lv_color_hex(0x1F2430), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(root, 0, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *label = lv_label_create(root);
        lv_label_set_text(label, "正在录音…  短按结束  上推取消");
        lv_obj_add_style(label, &s_style_body, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        s_overlays[OVERLAY_RECORDING].root = root;
        s_overlays[OVERLAY_RECORDING].label = label;
    }

    /* 提示条: 最低优先级, 底部 2 秒 */
    {
        lv_obj_t *root = lv_obj_create(parent);
        lv_obj_set_size(root, 320, 40);
        lv_obj_set_pos(root, 0, 440);
        lv_obj_set_style_bg_color(root, lv_color_hex(0x1F2430), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(root, 0, 0);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *label = lv_label_create(root);
        lv_label_set_text(label, "");
        lv_obj_add_style(label, &s_style_body, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

        s_overlays[OVERLAY_TOAST].root = root;
        s_overlays[OVERLAY_TOAST].label = label;
    }
}

/* ===================== 对外接口 ===================== */

void ui_pages_init(lv_obj_t *parent)
{
    ui_styles_init();
    ui_oobe_bg_init();
    ui_build_overlays(parent);

    int i;
    for (i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *root = ui_make_root(parent);
        s_pages[i].root = root;
        s_pages[i].status = ui_make_status(root, "                  ", &s_pages[i].status_label);
        s_pages[i].content = ui_make_content(root);
        s_pages[i].hint = ui_make_hint(root,
            (s_page_hints[i] != NULL) ? s_page_hints[i] : "");
        s_build_page = i;   /* 构建期间登记可选行 */
        s_item_count[i] = 0;
        s_cursor[i] = 0;
        if (s_page_builders[i] != NULL) {
            s_page_builders[i](s_pages[i].content);
        }
        s_build_page = -1;
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }

    for (i = 0; i < PAGE_BOOK_COUNT - PAGE_BOOKSHELF; i++) {
        lv_obj_t *root = ui_make_root(parent);
        s_book_pages[i].root = root;
        s_book_pages[i].status = ui_make_status(root, "                  ",
                                               &s_book_pages[i].status_label);
        s_book_pages[i].content = ui_make_content(root);
        s_book_pages[i].hint = ui_make_hint(root, s_book_hints[i]);
        if (s_book_builders[i] != NULL) {
            s_book_builders[i](s_book_pages[i].content);
        }
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ===================== 列表光标 ===================== */

static void ui_cursor_render(ui_page_t page)
{
    if (page < 0 || page >= (int)PAGE_COUNT) {
        return;
    }
    for (uint8_t i = 0; i < s_item_count[page]; i++) {
        if (s_items[page][i].label == NULL) {
            continue;
        }
        char buf[UI_ITEM_TEXT_MAX + 4];
        snprintf(buf, sizeof(buf), "%s%s", (i == s_cursor[page]) ? "> " : "  ",
                 s_items[page][i].text);
        lv_label_set_text(s_items[page][i].label, buf);
    }
}

/* ===================== 关机项（doc/02 §3.6 关机契约） ===================== */

/** 改设置页「关机」那一行的文案（行首标记由光标渲染统一补）。 */
static void ui_settings_shutdown_set_text(const char *text)
{
    if (s_item_count[PAGE_SETTINGS] <= UI_SETTINGS_SHUTDOWN_ITEM) {
        return;
    }
    ui_item_t *item = &s_items[PAGE_SETTINGS][UI_SETTINGS_SHUTDOWN_ITEM];
    if (item->label == NULL) {
        return;
    }
    snprintf(item->text, UI_ITEM_TEXT_MAX, "%s", text);
    ui_cursor_render(PAGE_SETTINGS);
}

void ui_pages_settings_shutdown_armed(bool armed)
{
    ui_settings_shutdown_set_text(armed ? "关机（长按确认）" : "关机");
}

void ui_pages_settings_shutdown_status(const char *text)
{
    if (text == NULL) {
        return;
    }
    ui_settings_shutdown_set_text(text);
}

int ui_pages_cursor_move(ui_page_t page, int delta)
{
    if (page < 0 || page >= (int)PAGE_COUNT || s_item_count[page] == 0) {
        return -1;
    }
    int c = (int)s_cursor[page] + delta;
    if (c < 0) {
        c = 0;
    }
    if (c > (int)s_item_count[page] - 1) {
        c = (int)s_item_count[page] - 1;
    }
    s_cursor[page] = (uint8_t)c;
    ui_cursor_render(page);
    return c;
}

int ui_pages_cursor_index(ui_page_t page)
{
    if (page < 0 || page >= (int)PAGE_COUNT || s_item_count[page] == 0) {
        return -1;
    }
    return (int)s_cursor[page];
}

void ui_pages_cursor_reset(ui_page_t page)
{
    if (page < 0 || page >= (int)PAGE_COUNT) {
        return;
    }
    s_cursor[page] = 0;
    ui_cursor_render(page);
}

/* ===================== 番茄钟（doc/03 §五 第 8 页） ===================== */

/* 首次用到时才去读卡（定义在“专注时长”段） */
static void ui_focus_load(void);

/** 刷新番茄钟页面上的大字与状态行。 */
static void ui_pomodoro_render(void)
{
    if (s_pomo_time_label == NULL) {
        return;
    }
    uint32_t sec = (s_pomo_remain_ms + 999u) / 1000u;
    char buf[24];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(sec / 60u),
             (unsigned long)(sec % 60u));
    lv_label_set_text(s_pomo_time_label, buf);

    if (s_pomo_sub_label == NULL) {
        return;
    }
    const char *sub = "专注中";
    if (s_pomo_state == POMO_PAUSED) {
        sub = "已暂停";
    } else if (s_pomo_state == POMO_DONE) {
        sub = "已完成，休息一下吧";
    } else if (s_pomo_state == POMO_IDLE) {
        sub = "准备开始";
    }
    lv_label_set_text(s_pomo_sub_label, sub);
}

void ui_pages_pomodoro_enter(void)
{
    /* 时长取本地设置（默认 25 分钟；已调过就直接用内存里的值） */
    ui_focus_load();
    int minutes = s_focus_minutes;
    if (minutes < UI_DATA_FOCUS_MIN) {
        minutes = UI_DATA_FOCUS_MIN;
    }
    if (minutes > UI_DATA_FOCUS_MAX) {
        minutes = UI_DATA_FOCUS_MAX;
    }
    s_pomo_total_ms = (uint32_t)minutes * 60000u;
    s_pomo_remain_ms = s_pomo_total_ms;
    s_pomo_state = POMO_RUNNING;
    s_pomo_tick_ready = false;
    ui_pomodoro_render();
}

void ui_pages_pomodoro_tick(uint32_t now_ms)
{
    if (s_pomo_state != POMO_RUNNING) {
        return;
    }
    if (!s_pomo_tick_ready) {
        /* 首帧只对齐基准，不计时间（暂停/继续后也一样） */
        s_pomo_last_ms = now_ms;
        s_pomo_tick_ready = true;
        return;
    }
    /* 毫秒差值（uint32 回绕安全），不做秒级累加 —— 免得长跑漂移 */
    uint32_t delta = now_ms - s_pomo_last_ms;
    s_pomo_last_ms = now_ms;
    if (delta == 0) {
        return;
    }
    if (delta >= s_pomo_remain_ms) {
        s_pomo_remain_ms = 0;
    } else {
        s_pomo_remain_ms -= delta;
    }
    if (s_pomo_remain_ms == 0) {
        s_pomo_state = POMO_DONE;
        s_pomo_tick_ready = false;
        /* 到点出声：只投递请求给音频任务，UI 不阻塞 */
        hal_audio_request_tone(880, 800);
    }
    ui_pomodoro_render();
}

void ui_pages_pomodoro_toggle(void)
{
    switch (s_pomo_state) {
    case POMO_RUNNING:
        s_pomo_state = POMO_PAUSED;
        s_pomo_tick_ready = false;
        break;
    case POMO_PAUSED:
        s_pomo_state = POMO_RUNNING;
        s_pomo_tick_ready = false;
        break;
    case POMO_DONE:
        /* 完成后再来一次短按 = 重新开始一轮 */
        s_pomo_remain_ms = s_pomo_total_ms;
        s_pomo_state = POMO_RUNNING;
        s_pomo_tick_ready = false;
        break;
    case POMO_IDLE:
    default:
        break;
    }
    ui_pomodoro_render();
}

void ui_pages_pomodoro_stop(void)
{
    s_pomo_state = POMO_IDLE;
    s_pomo_remain_ms = 0;
    s_pomo_tick_ready = false;
    ui_pomodoro_render();
}

/* ===================== 专注时长（PAGE_SETTINGS） ===================== */

/** 首次用到时才去读卡（读卡要走存储任务，不能在开机路径上干等）。 */
static void ui_focus_load(void)
{
    if (s_focus_loaded) {
        return;
    }
    s_focus_loaded = true;
    int minutes = UI_DATA_FOCUS_DEFAULT;
    if (ui_data_focus_minutes_get(&minutes) == UI_DATA_OK) {
        s_focus_minutes = minutes;
    }
    if (s_focus_minutes < UI_DATA_FOCUS_MIN) {
        s_focus_minutes = UI_DATA_FOCUS_MIN;
    }
    if (s_focus_minutes > UI_DATA_FOCUS_MAX) {
        s_focus_minutes = UI_DATA_FOCUS_MAX;
    }
    if (s_focus_value_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d 分钟", s_focus_minutes);
        lv_label_set_text(s_focus_value_label, buf);
    }
}

int ui_pages_focus_minutes(void)
{
    ui_focus_load();
    return s_focus_minutes;
}

void ui_pages_focus_adjust(int delta)
{
    int minutes = s_focus_minutes + (delta * UI_DATA_FOCUS_STEP);
    if (minutes < UI_DATA_FOCUS_MIN) {
        minutes = UI_DATA_FOCUS_MIN;
    }
    if (minutes > UI_DATA_FOCUS_MAX) {
        minutes = UI_DATA_FOCUS_MAX;
    }
    if (minutes == s_focus_minutes) {
        return;
    }
    s_focus_minutes = minutes;
    /* 落盘：原子写 + .bak 自愈（doc/04 §2.3），不放 NVS */
    ui_data_focus_minutes_set(minutes);
    if (s_focus_value_label != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d 分钟", s_focus_minutes);
        lv_label_set_text(s_focus_value_label, buf);
    }
}

/* ===================== 首页今日课程（读 /config/schedule.json） ===================== */

/** 星期几（中文）。入参是公历日期。 */
static const char *ui_weekday_cn(const ui_data_date_t *d)
{
    static const char *names[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = d->year;
    int m = d->month;
    if (m < 1 || m > 12) {
        return "";
    }
    if (m < 3) {
        y -= 1;
    }
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d->day) % 7;
    if (w < 0) {
        w += 7;
    }
    return names[w];
}

void ui_pages_refresh_home(void)
{
    if (s_home_schedule_label == NULL) {
        return;
    }
    static char buf[512];
    ui_data_date_t today;
    ui_data_lesson_t lessons[UI_DATA_MAX_LESSONS];
    uint32_t count = 0;

    int rc = UI_DATA_ERR_NOT_FOUND;
    if (ui_data_today(&today) == UI_DATA_OK) {
        rc = ui_data_read_schedule(&today, lessons, UI_DATA_MAX_LESSONS, &count);
    }
    if (rc != UI_DATA_OK) {
        lv_label_set_text(s_home_schedule_label, "今日课程\n暂时读不到课表");
        return;
    }
    if (count == 0) {
        snprintf(buf, sizeof(buf), "今日课程 (%s)\n今天没有课程安排", ui_weekday_cn(&today));
        lv_label_set_text(s_home_schedule_label, buf);
        return;
    }

    size_t pos = 0;
    int n = snprintf(buf + pos, sizeof(buf) - pos, "今日课程 (%s)\n", ui_weekday_cn(&today));
    pos += (n > 0) ? (size_t)n : 0;
    uint32_t i;
    for (i = 0; i < count && i < 4 && pos < sizeof(buf); i++) {
        n = snprintf(buf + pos, sizeof(buf) - pos, "%u.%s %s-%s\n",
                     (unsigned)(i + 1), lessons[i].subject, lessons[i].start,
                     lessons[i].end);
        if (n <= 0) {
            break;
        }
        pos += (size_t)n;
    }
    if (count > 4 && pos < sizeof(buf)) {
        snprintf(buf + pos, sizeof(buf) - pos, "……");
    }
    lv_label_set_text(s_home_schedule_label, buf);
}

/* ===================== 天气与城市（doc/05 §5.2） ===================== */

/**
 * 刷新首页的天气文案。
 *
 * `text` 是 ui_weather_display_text() 生成的成品文案（在线读数 / 缓存标注 / 空态）。
 * 本函数**不做任何判断** —— 拿不到数据时的「暂时拿不到天气」也是它传进来的。
 */
void ui_pages_set_weather_text(const char *text)
{
    if (s_home_weather_label == NULL) {
        return;
    }
    lv_label_set_text(s_home_weather_label, (text != NULL && text[0] != '\0') ? text
                                                                     : "暂时拿不到天气");
}

/**
 * 刷新设置页「所在城市」右侧的值。
 *
 * `zh` 是给用户看的中文名（候选表里的那一列）；没设置过时传 NULL → 显示「未设置」。
 */
void ui_pages_set_city_label(const char *zh)
{
    if (s_city_value_label == NULL) {
        return;
    }
    lv_label_set_text(s_city_value_label, (zh != NULL && zh[0] != '\0') ? zh : "未设置");
}

/* ===================== 书架与阅读页（doc/07） ===================== */

/* ===================== 阅读护栏：进度表与配置读写 ===================== */

/**
 * 读 /records/bookmarks.json；主文件坏了就退回 .bak（doc/07 §2.3 的自愈路径）。
 *
 * 只走 ui_data（存储任务请求/应答）—— UI 任务不碰 SPI（doc/02 §4.2）。
 */
static void ui_book_load_marks(void)
{
    uint32_t n1 = 0;
    uint32_t n2 = 0;

    s_bookmarks_buf[0] = '\0';
    s_bookmarks_bak_buf[0] = '\0';
    int r1 = ui_data_read_text(UI_DATA_PATH_BOOKMARKS, s_bookmarks_buf,
                               (uint32_t)sizeof(s_bookmarks_buf), &n1);
    int r2 = ui_data_read_text(UI_DATA_PATH_BOOKMARKS_BAK, s_bookmarks_bak_buf,
                               (uint32_t)sizeof(s_bookmarks_bak_buf), &n2);
    if (r1 < 0) {
        n1 = 0;
    }
    if (r2 < 0) {
        n2 = 0;
    }
    const char *json = s_bookmarks_buf;
    size_t len = (size_t)n1;
    if (ui_book_pick_source(s_bookmarks_buf, (size_t)n1, s_bookmarks_bak_buf, (size_t)n2,
                             &json, &len) < 0) {
        /* 两份都不可用：当作没有进度（不编造） */
        json = "";
        len = 0;
    }
    ui_bookmarks_parse(json, len, &s_bookmarks);
    if (s_bookmarks.schema_version == 0) {
        s_bookmarks.schema_version = 1;
    }
    s_bookmarks_loaded = true;
}

/** 读护栏配置（/config/profile.json）；读不到就用默认值（22:00 / 06:30 / 45 分钟）。 */
static void ui_book_load_guard(void)
{
    static char buf[1024];
    uint32_t n = 0;

    ui_book_guard_defaults(&s_book_guard);
    int rc = ui_data_read_text(UI_DATA_PATH_PROFILE, buf, (uint32_t)sizeof(buf), &n);
    if (rc >= 0 && n > 0) {
        ui_book_guard_parse(buf, (size_t)n, &s_book_guard);
    }
    s_book_guard_loaded = true;
}

/** 把进度表写回 /records/bookmarks.json（原子写：.tmp → .bak → 改名，doc/04 §2.3）。 */
static void ui_book_save_marks(void)
{
    int n = ui_bookmarks_format(&s_bookmarks, s_bookmarks_out, sizeof(s_bookmarks_out));
    if (n <= 0) {
        return;
    }
    ui_data_write_text(UI_DATA_PATH_BOOKMARKS, s_bookmarks_out, (uint32_t)n);
}

/** 记下当前这本书的进度并落盘（翻页停顿即自动保存 → 下次打开秒回原位）。 */
static void ui_book_save_progress(void)
{
    if (!s_bookmarks_loaded) {
        ui_book_load_marks();
    }
    if (s_reader_path[0] != '\0' && s_reader_size > 0) {
        const char *name = strrchr(s_reader_path, '/');
        name = (name != NULL) ? name + 1 : s_reader_path;
        ui_bookmarks_set(&s_bookmarks, name, s_reader_offset, s_reader_size);
    }
    ui_book_save_marks();
}

/**
 * 关机前把会话状态落盘：阅读进度 + 当日阅读时长（doc/07 §2.3 / §2.4）。
 *
 * 写盘走 ui_data_* 队列 → Core 0 存储任务完成（UI 核不碰 TF 卡，doc/02 §4.1）。
 */
void ui_pages_save_session_state(void)
{
    if (!s_bookmarks_loaded) {
        ui_book_load_marks();
    }
    ui_book_save_progress();
}

/** 跨日清零（doc/07 §2.4）：日期变了就把当日已读秒数清零。 */
static void ui_book_daily_rollover_check(void)
{
    char today[UI_DATA_MAX_DATE_STR];

    if (ui_data_today_str(today, sizeof(today)) != UI_DATA_OK) {
        return;   /* 没校时：不动计数器，也不编日期 */
    }
    if (ui_book_daily_rollover(&s_bookmarks, today)) {
        ui_book_save_marks();
    }
}

/** 把偏移对齐到 UTF-8 字符首字节（doc/07 §3.2：绝不切坏多字节字符）。 */
static uint32_t ui_reader_align_offset(const char *path, uint32_t offset)
{
    uint8_t buf[8];
    uint32_t got = 0;
    uint32_t start;

    if (path == NULL || offset < 4u) {
        return offset;
    }
    start = offset - 4u;
    if (ui_data_read_range(path, start, buf, (uint32_t)sizeof(buf), &got) != UI_DATA_OK ||
        got == 0) {
        return offset;
    }
    uint32_t rel = ui_data_utf8_align_forward(buf, got, 4u);
    if (rel >= got) {
        return offset;
    }
    return start + rel;
}

void ui_pages_set_book_locked_text(const char *title, const char *body)
{
    if (s_locked_title != NULL) {
        lv_label_set_text(s_locked_title, (title != NULL) ? title : "");
    }
    if (s_locked_body != NULL) {
        lv_label_set_text(s_locked_body, (body != NULL) ? body : "");
    }
}

bool ui_pages_reader_guard_check(void)
{
    int now_min = 0;

    if (!s_bookmarks_loaded) {
        ui_book_load_marks();
    }
    if (!s_book_guard_loaded) {
        ui_book_load_guard();
    }
    ui_book_daily_rollover_check();

    /* 宵禁：跨午夜区间按 now >= start || now < end 判（doc/07 §2.4） */
    if (ui_data_local_minutes(&now_min) == UI_DATA_OK &&
        ui_book_curfew_active(&s_book_guard.curfew, now_min)) {
        ui_pages_set_book_locked_text("该休息啦", "现在是阅读休息时段\n明早再来读书吧");
        return false;
    }

    /* 每日课外阅读时长上限 */
    if (ui_book_daily_limit_reached(&s_book_guard, s_bookmarks.daily_used_sec)) {
        ui_pages_set_book_locked_text("今天读得够多啦",
                                      "今天的课外阅读时间用完了\n明天再来读书吧");
        return false;
    }
    return true;
}

void ui_pages_reader_tick(uint32_t now_ms, bool active)
{
    if (!active || !s_reader_counting) {
        s_reader_tick_last_ms = now_ms;   /* 暂停：不累计 */
        return;
    }
    uint32_t delta = now_ms - s_reader_tick_last_ms;
    s_reader_tick_last_ms = now_ms;
    if (delta > 2000u) {
        return;   /* 停顿或休眠后醒来：不补计 */
    }
    s_reader_session_ms += delta;
    while (s_reader_session_ms >= 1000u) {
        s_reader_session_ms -= 1000u;
        s_bookmarks.daily_used_sec += 1u;
    }
}

void ui_pages_reader_leave(void)
{
    s_reader_counting = false;
    s_reader_session_ms = 0;
    ui_book_save_progress();   /* 退出阅读页也保存进度与当日时长 */
}

/** 重画书架光标标记。 */
static void ui_bookshelf_render_cursor(void)
{
    int i;
    for (i = 0; i < s_book_count; i++) {
        if (s_book_labels[i] == NULL) {
            continue;
        }
        char buf[UI_DATA_MAX_NAME + 4];
        snprintf(buf, sizeof(buf), "%s%s", (i == s_book_cursor) ? "> " : "  ",
                 s_book_names[i]);
        lv_label_set_text(s_book_labels[i], buf);
    }
}

void ui_pages_refresh_bookshelf(void)
{
    lv_obj_t *content = ui_page_content_book(PAGE_BOOKSHELF);
    if (content == NULL) {
        return;
    }
    lv_obj_clean(content);
    s_book_count = 0;
    s_book_cursor = 0;
    memset(s_book_labels, 0, sizeof(s_book_labels));

    lv_obj_t *title = ui_make_label(content, "我的书架", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    /* 进度表与护栏配置：书架要显示每本书的实时进度、今天还能读多久 */
    ui_book_load_marks();
    if (!s_book_guard_loaded) {
        ui_book_load_guard();
    }

    ui_data_file_t files[UI_DATA_MAX_FILES];
    uint32_t count = 0;
    int rc = ui_data_list_files(UI_DATA_DIR_BOOKS, files, UI_DATA_MAX_FILES, &count);
    if (rc != UI_DATA_OK) {
        lv_obj_t *empty = ui_make_label(content, "暂时读不到内容", false);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    uint32_t i;
    for (i = 0; i < count && s_book_count < UI_MAX_BOOKS; i++) {
        const char *name = files[i].name;
        size_t n = strlen(name);
        if (n < 5 || strcmp(name + n - 4, ".txt") != 0) {
            continue;   /* 只列正文，.toc 是配套目录 */
        }
        snprintf(s_book_names[s_book_count], UI_DATA_MAX_NAME, "%s", name);
        s_book_sizes[s_book_count] = files[i].size;
        char sub[24];
        int pct = -1;
        int idx = ui_bookmarks_find(&s_bookmarks, name);
        if (idx >= 0) {
            uint32_t total = files[i].size;
            if (total == 0) {
                total = s_bookmarks.items[idx].total_bytes;
            }
            /* 实时重算进度，绝不信 JSON 里的 percent（doc/07 §2.3） */
            pct = ui_book_progress_percent(s_bookmarks.items[idx].offset, total);
        }
        if (pct > 0) {
            snprintf(sub, sizeof(sub), "%d%%", pct);
        } else {
            snprintf(sub, sizeof(sub), "未读");
        }
        lv_obj_t *row = ui_make_list_item_ex(content, "  ", sub,
                                             &s_book_labels[s_book_count]);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 44 + s_book_count * 40);
        s_book_count++;
    }

    /* 状态栏：今天还能读多久（doc/07 §4.1） */
    lv_obj_t *status = s_book_pages[PAGE_BOOKSHELF - PAGE_BOOKSHELF].status_label;
    if (status != NULL) {
        char sbuf[32];
        if (s_book_guard.daily_limit_min > 0) {
            int used_min = (int)(s_bookmarks.daily_used_sec / 60u);
            int left = s_book_guard.daily_limit_min - used_min;
            if (left < 0) {
                left = 0;
            }
            snprintf(sbuf, sizeof(sbuf), "今日剩 %d 分", left);
        } else {
            snprintf(sbuf, sizeof(sbuf), "今日不限");
        }
        lv_label_set_text(status, sbuf);
    }
    if (s_book_count == 0) {
        lv_obj_t *empty = ui_make_label(content, "还没有下载内容", false);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    ui_bookshelf_render_cursor();
}

int ui_pages_bookshelf_move(int delta)
{
    if (s_book_count <= 0) {
        return -1;
    }
    int c = s_book_cursor + delta;
    if (c < 0) {
        c = 0;
    }
    if (c > s_book_count - 1) {
        c = s_book_count - 1;
    }
    s_book_cursor = c;
    ui_bookshelf_render_cursor();
    return c;
}

/**
 * 流式扫一遍同名 .toc（每行「章节标题|起始字节偏移量」，doc/07 §2.2）。
 *
 * 只记两样：每章在正文里的字节偏移 + 每章在 .toc 里的行首位置。
 * 章节标题**不常驻内存** —— 显示哪一页，才把那几行按位置读回来
 * （doc/07 §4.2 ④「每页显示 8 章，流式局部读取」）。
 *
 * 异常行整行丢弃（doc/07 §6 第 3 条）：无分隔符 / 标题为空 / 偏移量非十进制整数 /
 * 偏移量越过正文末尾；标题本身含 '|' 竖线照样能解析。
 */
static void ui_reader_load_toc(void)
{
    s_chapter_count = 0;
    s_chapter_title_idx = -1;
    s_chapter_title[0] = '\0';
    s_toc_page_cached = -1;
    memset(s_toc_titles, 0, sizeof(s_toc_titles));
    s_toc_path[0] = '\0';
    s_toc_size = 0;

    size_t n = strlen(s_reader_path);
    if (n < 4) {
        return;
    }
    snprintf(s_toc_path, sizeof(s_toc_path), "%s", s_reader_path);
    memcpy(s_toc_path + n - 4, ".toc", 5);

    if (ui_data_file_size(s_toc_path, &s_toc_size) != UI_DATA_OK || s_toc_size == 0) {
        return;
    }

    uint32_t pos = 0;
    uint32_t line_start = 0;   /* 当前行在 .toc 里的起始字节 */
    uint32_t line_len = 0;
    bool line_over = false;    /* 行过长 → 整行丢弃 */

    while (pos < s_toc_size && s_chapter_count < UI_MAX_CHAPTERS) {
        uint32_t want = (uint32_t)sizeof(s_toc_chunk);
        if (s_toc_size - pos < want) {
            want = s_toc_size - pos;
        }
        uint32_t got = 0;
        int rc = ui_data_read_range(s_toc_path, pos, s_toc_chunk, want, &got);
        if (rc < 0 || got == 0) {
            break;
        }
        for (uint32_t k = 0; k < got; k++) {
            char c = (char)s_toc_chunk[k];
            if (c == '\n') {
                if (!line_over && line_len > 0) {
                    ui_data_toc_item_t item;
                    if (ui_data_parse_toc(s_toc_line, line_len, s_reader_size, &item) == 1) {
                        s_chapter_offsets[s_chapter_count] = item.offset;
                        s_chapter_toc_pos[s_chapter_count] = line_start;
                        s_chapter_count++;
                    }
                }
                line_len = 0;
                line_over = false;
                line_start = pos + k + 1;
            } else if (line_len < (uint32_t)sizeof(s_toc_line) - 1) {
                s_toc_line[line_len++] = c;
            } else {
                line_over = true;
            }
        }
        pos += got;
    }
}

/**
 * 读回某一章的标题（按需从 .toc 读那一行，标题不常驻内存）。
 * 读不到就返回空串 —— 该章仍可跳转，只是没有名字。
 */
static void ui_toc_title_at(int idx, char *out, uint32_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (idx < 0 || idx >= s_chapter_count || s_toc_path[0] == '\0') {
        return;
    }
    uint32_t got = 0;
    int rc = ui_data_read_range(s_toc_path, s_chapter_toc_pos[idx], s_toc_readbuf,
                                (uint32_t)sizeof(s_toc_readbuf) - 1, &got);
    if (rc < 0 || got == 0) {
        return;
    }
    s_toc_readbuf[got] = '\0';
    uint32_t len = 0;
    while (len < got && s_toc_readbuf[len] != '\n') {
        len++;
    }
    ui_data_toc_item_t item;
    if (ui_data_parse_toc((const char *)s_toc_readbuf, len, s_reader_size, &item) == 1) {
        snprintf(out, cap, "%s", item.title);
    }
}

/** 当前所在章节标题（没有目录则返回空串）。 */
static const char *ui_current_chapter_title(void)
{
    if (s_chapter_count <= 0) {
        return "";
    }
    int idx = -1;
    for (int i = 0; i < s_chapter_count; i++) {
        if (s_chapter_offsets[i] <= s_reader_offset) {
            idx = i;
        } else {
            break;
        }
    }
    if (idx < 0) {
        return "";
    }
    if (idx != s_chapter_title_idx) {
        ui_toc_title_at(idx, s_chapter_title, (uint32_t)sizeof(s_chapter_title));
        s_chapter_title_idx = idx;
    }
    return s_chapter_title;
}

/** 渲染当前页 + 状态栏进度。 */
static int ui_reader_render(void)
{
    if (s_reader_body == NULL) {
        return -1;
    }
    static char buf[UI_DATA_PAGE_BYTES + 64];
    uint32_t consumed = 0;
    int lines = 0;
    int rc = ui_data_load_page(s_reader_path, s_reader_offset, &consumed, &lines, buf,
                                (uint32_t)sizeof(buf));
    if (rc != UI_DATA_OK) {
        s_reader_consumed = 0;
        lv_label_set_text(s_reader_body, "暂时读不到内容");
        return -1;
    }
    if (consumed == 0) {
        s_reader_consumed = 0;
        lv_label_set_text(s_reader_body, "已经读到最后一页");
        return 0;
    }
    s_reader_consumed = consumed;
    lv_label_set_text(s_reader_body, buf);

    lv_obj_t *status = s_book_pages[PAGE_READER - PAGE_BOOKSHELF].status_label;
    if (status != NULL) {
        /* 实时重算进度，绝不信 JSON 里的 percent（doc/07 §2.3） */
        int pct = ui_book_progress_percent(s_reader_offset, s_reader_size);
        char sbuf[64];
        const char *chapter = ui_current_chapter_title();
        if (chapter[0] != '\0') {
            snprintf(sbuf, sizeof(sbuf), "%s · %d%%", chapter, pct);
        } else {
            snprintf(sbuf, sizeof(sbuf), "已读 %d%%", pct);
        }
        lv_label_set_text(status, sbuf);
    }
    return 0;
}

int ui_pages_bookshelf_open_selected(void)
{
    if (s_book_cursor < 0 || s_book_cursor >= s_book_count) {
        return -1;
    }
    snprintf(s_reader_path, sizeof(s_reader_path), "%s/%s", UI_DATA_DIR_BOOKS,
             s_book_names[s_book_cursor]);
    s_reader_offset = 0;
    s_reader_consumed = 0;
    s_reader_history_count = 0;
    s_reader_size = 0;
    ui_data_file_size(s_reader_path, &s_reader_size);
    ui_reader_load_toc();

    /* 恢复上次进度：每本书只维护**唯一最新进度**（翻页停顿即自动保存） */
    if (!s_bookmarks_loaded) {
        ui_book_load_marks();
    }
    int idx = ui_bookmarks_find(&s_bookmarks, s_book_names[s_book_cursor]);
    if (idx >= 0 && s_bookmarks.items[idx].offset > 0) {
        uint32_t off = s_bookmarks.items[idx].offset;
        if (s_reader_size == 0 || off < s_reader_size) {
            s_reader_offset = ui_reader_align_offset(s_reader_path, off);
        }
    }

    s_reader_counting = true;
    s_reader_session_ms = 0;
    return ui_reader_render();
}

void ui_pages_reader_next_page(void)
{
    if (s_reader_consumed == 0) {
        ui_toast("已经读到最后一页");
        return;
    }
    if (s_reader_history_count < UI_READER_HISTORY) {
        s_reader_history[s_reader_history_count++] = s_reader_offset;
    } else {
        memmove(&s_reader_history[0], &s_reader_history[1],
                sizeof(uint32_t) * (size_t)(UI_READER_HISTORY - 1));
        s_reader_history[UI_READER_HISTORY - 1] = s_reader_offset;
    }
    s_reader_offset += s_reader_consumed;
    ui_reader_render();
    ui_book_save_progress();   /* 翻页后自动保存进度 */
}

void ui_pages_reader_prev_page(void)
{
    uint32_t target = 0;
    if (s_reader_history_count > 0) {
        target = s_reader_history[--s_reader_history_count];
    } else if (ui_data_prev_page_offset(s_reader_path, s_reader_offset, &target) != UI_DATA_OK) {
        return;
    }
    if (target >= s_reader_offset) {
        return;
    }
    s_reader_offset = target;
    ui_reader_render();
    ui_book_save_progress();   /* 翻页后自动保存进度 */
}

/* ===================== 阅读子页：菜单 / 章节目录 / 百分比定位（doc/07 §4.2 ③④⑤） =====================
 *
 * 三个子页共用一套结构：进页重建 → 手势改状态 → 再重建。
 *   PAGE_BOOK_MENU  快捷菜单（目录 / 百分比定位 / 亮度 / 返回书架）
 *   PAGE_BOOK_TOC   章节目录（8 章一页，左右翻页，短按跳章）
 *   PAGE_BOOK_JUMP  百分比定位（左右 ±1%，上下 ±10%，短按确认跳转）
 * 跳转落点一律走 ui_data_percent_to_offset() + UTF-8 首字节对齐（doc/07 §3.4）。
 * ================================================================================== */

/** 菜单页重建（doc/07 §4.2 ③）。 */
static void ui_book_menu_render(void)
{
    lv_obj_t *content = ui_page_content_book(PAGE_BOOK_MENU);
    if (content == NULL) {
        return;
    }
    lv_obj_clean(content);

    lv_obj_t *title = ui_make_label(content, "阅 读 功 能 菜 单", true);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    int i;
    for (i = 0; i < s_menu_count; i++) {
        char text[64];
        switch (s_menu_actions[i]) {
        case UI_BOOK_ACT_TOC:
            snprintf(text, sizeof(text), "章节目录跳转");
            break;
        case UI_BOOK_ACT_JUMP:
            snprintf(text, sizeof(text), "百分比进度定位");
            break;
        case UI_BOOK_ACT_BRIGHT:
            snprintf(text, sizeof(text), "屏幕亮度: %d / 5 档",
                     (int)hal_display_get_backlight());
            break;
        default:
            snprintf(text, sizeof(text), "返回书架");
            break;
        }
        char line[96];   /* 前缀 2 字节 + text 最多 63 字节 + 结尾 */
        snprintf(line, sizeof(line), "%s%s", (i == s_book_menu_sel) ? "> " : "  ", text);
        lv_obj_t *row = ui_make_label(content, line, false);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 56 + i * 40);
    }
}

/** 把可能落在字符中间的偏移对齐到该字符的首字节（doc/07 §3.4）。 */
static uint32_t ui_reader_seek_align(uint32_t offset)
{
    if (offset < 4u) {
        return offset;
    }
    uint8_t buf[8];
    uint32_t got = 0;
    if (ui_data_read_range(s_reader_path, offset - 4u, buf, (uint32_t)sizeof(buf), &got) < 0 ||
        got <= 4u) {
        return offset;
    }
    uint32_t rel = ui_data_utf8_align_backward(buf, got, 4u);
    return (offset - 4u) + rel;
}

/** 跳到正文的某个字节偏移：对齐 → 重画 → 存进度。返回新偏移，失败返回 -1。 */
static int ui_reader_seek(uint32_t offset)
{
    if (s_reader_size == 0) {
        return -1;
    }
    if (offset >= s_reader_size) {
        offset = s_reader_size - 1;
    }
    uint32_t aligned = ui_reader_seek_align(offset);
    if (aligned >= s_reader_size) {
        aligned = s_reader_size - 1;
    }
    s_reader_offset = aligned;
    s_reader_history_count = 0;   /* 跳走后旧页起点栈作废 */
    s_reader_consumed = 0;
    ui_reader_render();
    ui_book_save_progress();
    return (int)aligned;
}

/** 按页读回章节标题（标题不常驻内存，只留当前页 8 个）。 */
static void ui_book_toc_load_page_titles(int page)
{
    memset(s_toc_titles, 0, sizeof(s_toc_titles));
    int first = page * UI_TOC_PER_PAGE;
    if (first >= s_chapter_count) {
        s_toc_page_cached = page;
        return;
    }
    /* 一次范围读回本页的整段行，再逐行判定 —— 避免每章一次读 */
    uint32_t start = s_chapter_toc_pos[first];
    uint32_t end = s_toc_size;
    if (first + UI_TOC_PER_PAGE < s_chapter_count) {
        end = s_chapter_toc_pos[first + UI_TOC_PER_PAGE];
    }
    uint32_t len = end - start;
    if (len > (uint32_t)sizeof(s_toc_readbuf) - 1u) {
        len = (uint32_t)sizeof(s_toc_readbuf) - 1u;
    }
    uint32_t got = 0;
    if (ui_data_read_range(s_toc_path, start, s_toc_readbuf, len, &got) < 0 || got == 0) {
        s_toc_page_cached = page;
        return;
    }
    s_toc_readbuf[got] = '\0';
    uint32_t i = 0;
    int n = 0;
    while (i < got && n < UI_TOC_PER_PAGE) {
        uint32_t j = i;
        while (j < got && s_toc_readbuf[j] != '\n') {
            j++;
        }
        if (j >= got) {
            break;   /* 末行不完整 → 不解析 */
        }
        ui_data_toc_item_t item;
        if (ui_data_parse_toc((const char *)&s_toc_readbuf[i], j - i, s_reader_size, &item) == 1) {
            memcpy(s_toc_titles[n], item.title, sizeof(item.title));
            n++;
        }
        i = (j < got) ? (j + 1u) : got;
    }
    s_toc_page_cached = page;
}

/** 目录页重建（doc/07 §4.2 ④）。 */
static void ui_book_toc_render(void)
{
    lv_obj_t *content = ui_page_content_book(PAGE_BOOK_TOC);
    if (content == NULL) {
        return;
    }
    lv_obj_clean(content);

    int pages = (s_chapter_count + UI_TOC_PER_PAGE - 1) / UI_TOC_PER_PAGE;
    if (pages < 1) {
        pages = 1;
    }
    if (s_book_toc_page >= pages) {
        s_book_toc_page = pages - 1;
    }
    if (s_book_toc_page < 0) {
        s_book_toc_page = 0;
    }

    char head[64];
    snprintf(head, sizeof(head), "章节目录 (共 %d 章)", s_chapter_count);
    lv_obj_t *title = ui_make_label(content, head, true);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char pagetxt[40];
    snprintf(pagetxt, sizeof(pagetxt), "%d/%d 页", s_book_toc_page + 1, pages);
    lv_obj_t *pg = ui_make_label(content, pagetxt, false);
    lv_obj_align(pg, LV_ALIGN_TOP_RIGHT, 0, 0);

    if (s_chapter_count == 0) {
        lv_obj_t *empty = ui_make_label(content, "这本书没有章节", false);
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 0, 56);
        return;
    }

    int first = s_book_toc_page * UI_TOC_PER_PAGE;
    for (int i = 0; i < UI_TOC_PER_PAGE; i++) {
        int idx = first + i;
        if (idx >= s_chapter_count) {
            break;
        }
        char line[64];
        snprintf(line, sizeof(line), "%s%s", (idx == s_book_toc_sel) ? "> " : "  ",
                 s_toc_titles[i]);
        lv_obj_t *row = ui_make_label(content, line, false);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 56 + i * 40);
    }
}

/** 百分比定位页重建（doc/07 §4.2 ⑤）。 */
static void ui_book_jump_render(void)
{
    lv_obj_t *content = ui_page_content_book(PAGE_BOOK_JUMP);
    if (content == NULL) {
        return;
    }
    lv_obj_clean(content);

    lv_obj_t *title = ui_make_label(content, "进度定位跳转", true);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char pcttxt[40];
    snprintf(pcttxt, sizeof(pcttxt), "目标阅读进度: %d%%", s_book_jump_pct);
    lv_obj_t *pct = ui_make_label(content, pcttxt, false);
    lv_obj_align(pct, LV_ALIGN_TOP_MID, 0, 90);

    char bar[UI_JUMP_BLOCKS * 3 + 1];
    int fill = (s_book_jump_pct * UI_JUMP_BLOCKS + 50) / 100;
    for (int i = 0; i < UI_JUMP_BLOCKS; i++) {
        memcpy(bar + i * 3, (i < fill) ? "█" : "░", 3);
    }
    bar[UI_JUMP_BLOCKS * 3] = '\0';
    lv_obj_t *barlabel = ui_make_label(content, bar, false);
    lv_obj_align(barlabel, LV_ALIGN_TOP_MID, 0, 130);

    lv_obj_t *hint = ui_make_label(content, "提示: 左右推微调1%，上下推10%", false);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 175);
    lv_obj_t *hint2 = ui_make_label(content, "短按确认跳转", false);
    lv_obj_align(hint2, LV_ALIGN_TOP_MID, 0, 200);
}

/* ---------- 三个子页的公开操作（ui_router 依此分发手势） ---------- */

void ui_pages_book_menu_enter(void)
{
    s_book_menu_sel = 0;
    s_menu_count = 0;
    if (s_chapter_count > 0) {
        s_menu_actions[s_menu_count++] = UI_BOOK_ACT_TOC;
    }
    s_menu_actions[s_menu_count++] = UI_BOOK_ACT_JUMP;
    s_menu_actions[s_menu_count++] = UI_BOOK_ACT_BRIGHT;
    s_menu_actions[s_menu_count++] = UI_BOOK_ACT_BACK;
    ui_book_menu_render();
}

int ui_pages_book_menu_move(int delta)
{
    if (s_menu_count <= 0) {
        return 0;
    }
    int next = s_book_menu_sel + delta;
    if (next < 0) {
        next = s_menu_count - 1;
    }
    if (next >= s_menu_count) {
        next = 0;
    }
    s_book_menu_sel = next;
    ui_book_menu_render();
    return next;
}

int ui_pages_book_menu_select(void)
{
    if (s_book_menu_sel < 0 || s_book_menu_sel >= s_menu_count) {
        return UI_BOOK_ACT_BACK;
    }
    return s_menu_actions[s_book_menu_sel];
}

void ui_pages_book_menu_refresh(void)
{
    ui_book_menu_render();
}

void ui_pages_book_toc_enter(void)
{
    s_book_toc_sel = 0;
    s_book_toc_page = 0;
    ui_book_toc_load_page_titles(0);
    ui_book_toc_render();
}

int ui_pages_book_toc_move(int delta)
{
    if (s_chapter_count <= 0) {
        return -1;
    }
    int sel = s_book_toc_sel + delta;
    if (sel < 0) {
        sel = 0;
    }
    if (sel > s_chapter_count - 1) {
        sel = s_chapter_count - 1;
    }
    s_book_toc_sel = sel;
    int page = sel / UI_TOC_PER_PAGE;
    if (page != s_book_toc_page) {
        s_book_toc_page = page;
        ui_book_toc_load_page_titles(page);
    }
    ui_book_toc_render();
    return sel;
}

int ui_pages_book_toc_page(int delta)
{
    int pages = (s_chapter_count + UI_TOC_PER_PAGE - 1) / UI_TOC_PER_PAGE;
    if (pages < 1) {
        pages = 1;
    }
    int page = s_book_toc_page + delta;
    if (page < 0) {
        page = 0;
    }
    if (page > pages - 1) {
        page = pages - 1;
    }
    s_book_toc_page = page;
    int first = page * UI_TOC_PER_PAGE;
    int last = first + UI_TOC_PER_PAGE - 1;
    if (last > s_chapter_count - 1) {
        last = s_chapter_count - 1;
    }
    if (s_book_toc_sel < first) {
        s_book_toc_sel = first;
    }
    if (s_book_toc_sel > last) {
        s_book_toc_sel = last;
    }
    /* 换页 → 重新读回这一页的标题 */
    if (page != s_toc_page_cached) {
        ui_book_toc_load_page_titles(page);
    }
    ui_book_toc_render();
    return page;
}

int ui_pages_book_toc_select(void)
{
    if (s_book_toc_sel < 0 || s_book_toc_sel >= s_chapter_count) {
        return -1;
    }
    return ui_reader_seek(s_chapter_offsets[s_book_toc_sel]);
}

void ui_pages_book_jump_enter(void)
{
    int pct = ui_book_progress_percent(s_reader_offset, s_reader_size);
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    s_book_jump_pct = pct;
    ui_book_jump_render();
}

void ui_pages_book_jump_adjust(int delta)
{
    int pct = s_book_jump_pct + delta;
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    s_book_jump_pct = pct;
    ui_book_jump_render();
}

int ui_pages_book_jump_select(void)
{
    if (s_reader_size == 0) {
        return -1;
    }
    uint32_t raw = ui_data_percent_to_offset(s_reader_size, (uint32_t)s_book_jump_pct);
    return ui_reader_seek(raw);
}

/* ===================== 状态栏 ===================== */

void ui_pages_set_status(ui_page_t page, const char *text)
{
    if (page < 0 || page >= (int)PAGE_COUNT) {
        return;
    }
    if (s_pages[page].status_label == NULL) {
        return;
    }
    lv_label_set_text(s_pages[page].status_label, (text != NULL) ? text : "");
}

/* ===================== 解析结果页 ===================== */

/** 按行切出正文的一屏（从 s_solve_scroll 行起，最多 UI_SOLVE_LINES 行）。 */
static void ui_solve_render(void)
{
    if (s_solve_body == NULL) {
        return;
    }
    static char buf[UI_SOLVE_TEXT_MAX];   /* static：别占 FreeRTOS 任务栈 */
    size_t pos = 0;
    const char *p = s_solve_text;
    int line = 0;
    while (*p != '\0' && line < s_solve_scroll) {
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
        line++;
    }
    int shown = 0;
    while (*p != '\0' && shown < UI_SOLVE_LINES) {
        const char *e = p;
        while (*e != '\0' && *e != '\n') {
            e++;
        }
        size_t n = (size_t)(e - p);
        if (pos + n + 2 >= sizeof(buf)) {
            break;
        }
        memcpy(buf + pos, p, n);
        pos += n;
        buf[pos++] = '\n';
        shown++;
        p = (*e == '\n') ? e + 1 : e;
    }
    buf[pos] = '\0';
    lv_label_set_text(s_solve_body, buf);
}

void ui_pages_set_solve_result(const char *markdown, const char *question)
{
    snprintf(s_solve_text, sizeof(s_solve_text), "%s", (markdown != NULL) ? markdown : "");
    s_solve_scroll = 0;
    ui_solve_render();
    if (s_solve_question != NULL) {
        lv_label_set_text(s_solve_question, (question != NULL) ? question : "还有哪里没想明白？");
    }
}

bool ui_pages_solve_scroll(int delta)
{
    int total = 1;
    for (const char *p = s_solve_text; *p != '\0'; p++) {
        if (*p == '\n') {
            total++;
        }
    }
    int max_scroll = total - UI_SOLVE_LINES;
    if (max_scroll < 0) {
        max_scroll = 0;
    }
    int next = s_solve_scroll + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > max_scroll) {
        next = max_scroll;
    }
    if (next == s_solve_scroll) {
        return false;
    }
    s_solve_scroll = next;
    ui_solve_render();
    return true;
}

lv_obj_t *ui_page_root(ui_page_t page)
{
    if (page >= PAGE_COUNT) {
        return NULL;
    }
    return s_pages[page].root;
}

lv_obj_t *ui_page_content(ui_page_t page)
{
    if (page >= PAGE_COUNT) {
        return NULL;
    }
    return s_pages[page].content;
}

lv_obj_t *ui_page_root_book(ui_book_page_t page)
{
    int idx = (int)page - (int)PAGE_BOOKSHELF;
    if (idx < 0 || idx >= PAGE_BOOK_COUNT - PAGE_BOOKSHELF) {
        return NULL;
    }
    return s_book_pages[idx].root;
}

lv_obj_t *ui_page_content_book(ui_book_page_t page)
{
    int idx = (int)page - (int)PAGE_BOOKSHELF;
    if (idx < 0 || idx >= PAGE_BOOK_COUNT - PAGE_BOOKSHELF) {
        return NULL;
    }
    return s_book_pages[idx].content;
}

lv_obj_t *ui_overlay_root(ui_overlay_t overlay)
{
    if (overlay >= OVERLAY_COUNT) {
        return NULL;
    }
    return s_overlays[overlay].root;
}

void ui_overlay_set_text(ui_overlay_t overlay, const char *text)
{
    if (overlay >= OVERLAY_COUNT || s_overlays[overlay].label == NULL) {
        return;
    }
    lv_label_set_text(s_overlays[overlay].label, text);
}

void ui_overlay_set_visible(ui_overlay_t overlay, bool visible)
{
    if (overlay >= OVERLAY_COUNT || s_overlays[overlay].root == NULL) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_overlays[overlay].root, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overlays[overlay].root, LV_OBJ_FLAG_HIDDEN);
    }
}
