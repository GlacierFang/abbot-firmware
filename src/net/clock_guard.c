/**
 * clock_guard.c —— 时钟可信度熔断策略（doc/04 §1.3）
 *
 * 纯 C，无 Arduino 依赖 —— 主机侧可直接编译运行单测。
 */
#include "clock_guard.h"

static bool s_unverified = false;
static uint64_t s_last_ntp_epoch = 0;
static uint64_t s_last_checkpoint_epoch = 0;

bool clock_guard_should_fuse(uint64_t now_epoch, uint64_t last_ntp_epoch, uint32_t threshold_days)
{
    if (now_epoch < CLOCK_GUARD_MIN_VALID_EPOCH) {
        return true; /* 系统时间本身不可信 —— 掉电后 RTC 归零 */
    }
    if (last_ntp_epoch == 0) {
        return true; /* 从未校准过 */
    }
    uint64_t diff = (now_epoch > last_ntp_epoch) ? (now_epoch - last_ntp_epoch)
                                                  : (last_ntp_epoch - now_epoch);
    return diff > (uint64_t)threshold_days * 86400ull;
}

void clock_guard_boot(uint64_t now_epoch, uint64_t last_ntp_epoch, uint32_t threshold_days)
{
    s_last_ntp_epoch = last_ntp_epoch;
    s_unverified = clock_guard_should_fuse(now_epoch, last_ntp_epoch, threshold_days);
}

bool clock_guard_is_unverified(void)
{
    return s_unverified;
}

uint64_t clock_guard_last_ntp_epoch(void)
{
    return s_last_ntp_epoch;
}

void clock_guard_mark_verified(uint64_t now_epoch)
{
    s_unverified = false;
    s_last_ntp_epoch = now_epoch;
    s_last_checkpoint_epoch = now_epoch;
}

bool clock_guard_checkpoint_due(uint64_t now_epoch)
{
    if (s_last_checkpoint_epoch == 0) {
        return true;
    }
    uint64_t cur_bucket = now_epoch / CLOCK_GUARD_HOUR_SECONDS;
    uint64_t last_bucket = s_last_checkpoint_epoch / CLOCK_GUARD_HOUR_SECONDS;
    return cur_bucket != last_bucket;
}

void clock_guard_mark_checkpoint(uint64_t now_epoch)
{
    s_last_checkpoint_epoch = now_epoch;
}

uint64_t clock_guard_last_checkpoint_epoch(void)
{
    return s_last_checkpoint_epoch;
}

/* Howard Hinnant 的 days_from_civil —— 公历转天数，无平台依赖。 */
static int64_t clock_days_from_civil(int y, int m, int d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);              /* [0, 399] */
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

uint64_t clock_guard_epoch_from_local(int year, int month, int day, int hour, int minute,
                                       int32_t tz_offset_sec)
{
    if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return 0;
    }
    int64_t days = clock_days_from_civil(year, month, day);
    int64_t local_sec = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60;
    int64_t utc_sec = local_sec - (int64_t)tz_offset_sec;
    if (utc_sec <= 0) {
        return 0;
    }
    return (uint64_t)utc_sec;
}
