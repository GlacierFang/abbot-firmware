/**
 * test_ui_shutdown_state.c —— 关机状态机的主机侧单测
 *
 * 为什么单独测：关机是**不可逆**操作（深睡后要摇杆长按才醒）。
 * 状态转换规则必须能在主机上被逐条验证，不能只靠真机手点。
 *
 * 覆盖的不变式：
 *   1. IDLE 下不能直接确认关机（防误触的第一道闸）；
 *   2. 确认态下重复 arm 无效（不重复进确认流程）；
 *   3. 确认态可取消；
 *   4. RUNNING 后既不能重复确认也不能取消（关机一旦开始就不可逆）。
 *
 * 用法：
 *   gcc -Wall -Wextra -Werror -I include -I src \
 *       test/test_ui_shutdown_state.c src/ui/ui_shutdown_state.c \
 *       -o /tmp/test_ui_shutdown_state
 */
#include <stdio.h>

#include "ui_shutdown_state.h"

static int g_fail = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            g_fail++;                                                   \
        }                                                               \
    } while (0)

int main(void)
{
    /* 初始状态 */
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_IDLE);

    /* 不变式 1：没进确认态就不许确认关机 */
    CHECK(ui_shutdown_state_confirm() == false);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_IDLE);

    /* IDLE 下 disarm 也没有意义 */
    CHECK(ui_shutdown_state_disarm() == false);

    /* 进确认态 */
    CHECK(ui_shutdown_state_arm() == true);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_ARMED);

    /* 不变式 2：确认态下重复 arm 无效 */
    CHECK(ui_shutdown_state_arm() == false);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_ARMED);

    /* 确认关机 -> RUNNING */
    CHECK(ui_shutdown_state_confirm() == true);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_RUNNING);

    /* 不变式 4：RUNNING 之后既不能重复确认，也不能取消 */
    CHECK(ui_shutdown_state_confirm() == false);
    CHECK(ui_shutdown_state_disarm() == false);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_RUNNING);

    /* reset 回到初始态（开机初始化用） */
    ui_shutdown_state_reset();
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_IDLE);

    /* 不变式 3：确认态可取消（回到普通设置菜单） */
    CHECK(ui_shutdown_state_arm() == true);
    CHECK(ui_shutdown_state_disarm() == true);
    CHECK(ui_shutdown_state_get() == UI_SHUTDOWN_IDLE);

    if (g_fail == 0) {
        printf("ui_shutdown_state: all checks passed\n");
        return 0;
    }
    printf("ui_shutdown_state: %d check(s) FAILED\n", g_fail);
    return 1;
}
