/**
 * clock_guard.h —— 时钟可信度熔断（doc/04 §1.3）
 *
 * 纯 C，不依赖 Arduino.h —— 可在主机侧编译与单测（与 cards.c / net_req.c 同一套路）。
 *
 * 职责：
 *   1. 开机判定：当前系统时间是否可信（g_clock_unverified 语义）；
 *   2. 整点低磨损检查点：判断「这一小时该不该写一次 sys_clock」；
 *   3. 校时成功后的解熔断；
 *   4. 本地墙钟 → UTC Unix 时间戳换算（手动校时用）。
 *
 * 熔断语义（doc/04 §1.3）：
 *   - 从未校时（last_ntp == 0）→ 熔断；
 *   - 系统时间本身不可信（早于 2023-11-14，说明掉电后 RTC 归零）→ 熔断；
 *   - 与上次成功校时相差超过阈值天数（默认 7 天）→ 熔断。
 * 熔断期间：离线功能照常，只暂缓加密端云业务请求（由 net_api 层拦截）。
 */
#ifndef ABBOT_CLOCK_GUARD_H
#define ABBOT_CLOCK_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** doc/04 §1.3：距上次校时超过 7 天（或从未校准）即熔断。 */
#define CLOCK_GUARD_UNVERIFIED_DAYS 7u

/** 一小时的秒数（整点检查点粒度）。 */
#define CLOCK_GUARD_HOUR_SECONDS 3600ull

/** 早于这个时刻说明系统时间不可信（掉电后 RTC 归零 / 从未校时）。 */
#define CLOCK_GUARD_MIN_VALID_EPOCH 1700000000ull

bool clock_guard_should_fuse(uint64_t now_epoch, uint64_t last_ntp_epoch, uint32_t threshold_days);
void clock_guard_boot(uint64_t now_epoch, uint64_t last_ntp_epoch, uint32_t threshold_days);
bool clock_guard_is_unverified(void);
uint64_t clock_guard_last_ntp_epoch(void);
void clock_guard_mark_verified(uint64_t now_epoch);
bool clock_guard_checkpoint_due(uint64_t now_epoch);
void clock_guard_mark_checkpoint(uint64_t now_epoch);
uint64_t clock_guard_last_checkpoint_epoch(void);

/** 本地墙钟（UTC+8）→ UTC Unix 时间戳。非法输入返回 0。 */
uint64_t clock_guard_epoch_from_local(int year, int month, int day, int hour, int minute,
                                       int32_t tz_offset_sec);

#ifdef __cplusplus
}
#endif

#endif /* ABBOT_CLOCK_GUARD_H */
