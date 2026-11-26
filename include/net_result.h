/**
 * net_result.h —— 云端业务结果三态判定（doc/05 §6）
 *
 * 这套判定原先住在离线队列模块 outbox.h 里。在线派发同样要用，
 * 但它跟「离线补传」没有任何关系 —— 所以单独成文件，不带 outbox 命名。
 *
 *   DONE  —— 云端收下并处理完（biz=0）
 *   DROP  —— 明确拒收且重试无用（4003 没听清 / 4004 学段未设 / 4291 配额用完）
 *   RETRY —— 暂时性故障（4290 并发被拒 / 5001 上游故障 / 未知码 / -1）
 */
#ifndef ABBOT_NET_RESULT_H
#define ABBOT_NET_RESULT_H

#define NET_RESULT_DONE 0
#define NET_RESULT_DROP 1
#define NET_RESULT_RETRY 2
#define NET_BIZ_ASR_FAILED 4003
#define NET_BIZ_PROFILE 4004
#define NET_BIZ_QUOTA 4291

static inline int net_classify_result(int biz_code)
{
    switch (biz_code) {
    case 0:
        return NET_RESULT_DONE;
    case NET_BIZ_ASR_FAILED:
    case NET_BIZ_PROFILE:
    case NET_BIZ_QUOTA:
        return NET_RESULT_DROP;
    default:
        /* 4290 并发被拒 / 5001 上游故障 / 未知码 / -1 传输失败 → 可重试。 */
        return NET_RESULT_RETRY;
    }
}

#endif /* ABBOT_NET_RESULT_H */
