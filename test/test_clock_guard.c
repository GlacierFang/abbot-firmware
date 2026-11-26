/**
 * test_clock_guard.c —— 时钟熔断策略主机侧单测（doc/04 §1.3）
 *
 * 纯 C，不依赖 Arduino.h：
 *   gcc -Wall -Wextra -Werror -I include -I test/host_stubs \
 *       test/test_clock_guard.c src/net/clock_guard.c -o /tmp/t_clock && /tmp/t_clock
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "clock_guard.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                        \
    do {                                                                        \
        if (!(cond)) {                                                          \
            printf("FAIL: %s (%s:%d)\n", (msg), __FILE__, __LINE__);            \
            g_fail++;                                                           \
        }                                                                       \
    } while (0)

int main(void)
{
    const uint64_t now = 1795651200ull; /* 2026-11-26 00:00:00 UTC */

    CHECK(clock_guard_should_fuse(1600000000ull, now, 7), "insane clock must fuse");
    CHECK(clock_guard_should_fuse(now, 0ull, 7), "never synced must fuse");
    CHECK(!clock_guard_should_fuse(now, now - 3ull * 86400ull, 7), "3 days must not fuse");
    CHECK(clock_guard_should_fuse(now, now - 8ull * 86400ull, 7), "8 days must fuse");
    CHECK(clock_guard_should_fuse(now - 8ull * 86400ull, now, 7), "clock behind must fuse");
    CHECK(clock_guard_should_fuse(now + 8ull * 86400ull, now, 7), "clock ahead must fuse");

    clock_guard_boot(now, 0ull, CLOCK_GUARD_UNVERIFIED_DAYS);
    CHECK(clock_guard_is_unverified(), "boot without ntp must be unverified");

    clock_guard_mark_verified(now);
    CHECK(!clock_guard_is_unverified(), "verified must clear the fuse");
    CHECK(clock_guard_last_ntp_epoch() == now, "last ntp must be recorded");

    clock_guard_mark_checkpoint(now);
    CHECK(!clock_guard_checkpoint_due(now + 600ull), "same hour must not be due");
    CHECK(clock_guard_checkpoint_due(now + 3600ull), "next hour must be due");

    uint64_t e = clock_guard_epoch_from_local(2026, 11, 26, 8, 0, 8 * 3600);
    CHECK(e == now, "local time must convert to utc epoch");
    CHECK(clock_guard_epoch_from_local(2026, 13, 26, 8, 0, 8 * 3600) == 0, "invalid month");
    CHECK(clock_guard_epoch_from_local(2026, 11, 26, 25, 0, 8 * 3600) == 0, "invalid hour");
    CHECK(clock_guard_epoch_from_local(2026, 11, 26, 8, 61, 8 * 3600) == 0, "invalid minute");

    if (g_fail == 0) {
        printf("OK: clock_guard 全部通过\n");
        return 0;
    }
    printf("FAILED: %d 项未通过\n", g_fail);
    return 1;
}
