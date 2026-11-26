/**
 * voice_capture.h —— 设备端语音录音生产者（doc/03 §3.1 录音浮层）
 *
 * 职责边界：
 *   - 本模块是**录音生产者**：采集 PCM → 投递 NET_OP_VOICE（PCM 留在内存里）。
 *   - 不落盘、不入队：没有离线补传（2026-11-26 定）。
 *   - 服务端零改动：仍复用 /api/v1/study/voice。
 *
 * 为什么不落盘：
 *   请求一旦发出就没法反悔，但**重录的成本可以接受**（2026-11-26 定）。
 *   失败就让用户重说一遍，不做暂存队列。
 *
 * 线程约定（doc/02 §4.1 双核铁律）：
 *   - UI 核（Core 1）只调 start/stop/cancel/interrupt 设置意图标志，**不做 TF 卡 IO**。
 *   - 音频任务（Core 0）通过 voice_capture_audio_poll() 驱动采集与收尾，
 *     所有落盘 / 队列投递都在 Core 0 完成。
 */
#ifndef VOICE_CAPTURE_H
#define VOICE_CAPTURE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= 纯逻辑（主机侧可测，不依赖硬件） ================= */

/** 采样率：16kHz（doc/04 §3.5 语音契约）。 */
#define VOICE_SAMPLE_RATE 16000
/** 单次录音最长 10 秒（doc/03 §3.1）。 */
#define VOICE_MAX_SECONDS 10
/** 采样点上限。 */
#define VOICE_MAX_SAMPLES (VOICE_SAMPLE_RATE * VOICE_MAX_SECONDS)
/** 有效时长下限 0.5 秒 —— 与云端 ASR 的判定同口径（短于此云端返回 4003）。 */
#define VOICE_MIN_SAMPLES (VOICE_SAMPLE_RATE / 2)
/** 提醒打断时的存队下限 1.0 秒（doc/03 §3.1 打断与保护恢复铁律）。 */
#define VOICE_INTERRUPT_MIN_SAMPLES VOICE_SAMPLE_RATE
/** PSRAM 余量门槛：录音缓冲之外再留的余量（doc/03 ERR_PSRAM_OOM）。 */
#define VOICE_PSRAM_HEADROOM (32 * 1024)

/** PCM 字节数（16bit/Mono 裸 PCM = 采样点数 × 2）。 */
static inline size_t voice_pcm_bytes(size_t samples)
{
    return samples * 2u;
}

/** 录音需要的 PSRAM 字节数（缓冲本体 + 余量）。 */
static inline size_t voice_psram_need(void)
{
    return voice_pcm_bytes(VOICE_MAX_SAMPLES) + VOICE_PSRAM_HEADROOM;
}

/** 时长是否够（够才值得发云端）。 */
static inline bool voice_samples_enough(size_t samples)
{
    return samples >= (size_t)VOICE_MIN_SAMPLES;
}

/** 峰值音量 → 0~5 档（浮层波形用）。 */
static inline uint8_t voice_level_of(int32_t peak)
{
    if (peak < 200) {
        return 0;
    }
    if (peak < 1500) {
        return 1;
    }
    if (peak < 4000) {
        return 2;
    }
    if (peak < 9000) {
        return 3;
    }
    if (peak < 16000) {
        return 4;
    }
    return 5;
}

/* ================= 录音上下文 ================= */

/** 录音上下文（宽度与 net_request_t 的对应字段保持一致）。 */
typedef struct {
    char page[16];    /**< 当前页面，如 "CARD_STUDY"；空串表示首页提问 */
    char pack_id[24]; /**< 学科包 id（可空） */
    char card_id[24]; /**< 卡片 id（可空） */
} voice_context_t;

/* ================= 运行时接口 ================= */

/**
 * 开始一次录音（UI 核调用）。
 *
 * @param ctx 录音发生时的页面上下文（复制一份，不持有调用方指针）
 * @return true 已受理；false 正在录音或内存不足
 */
bool voice_capture_start(const voice_context_t *ctx);

/** 结束录音并提交（UI 核调用；落盘与投递在 Core 0 完成）。 */
void voice_capture_stop(void);

/** 取消录音：不落盘、不投递、丢弃已采数据（UI 核调用）。 */
void voice_capture_cancel(void);

/**
 * 提醒弹窗打断录音（UI 核调用）。
 * 有效时长 >= 1.0 秒 → 存进待问队列；不足 → 直接丢弃（doc/03 §3.1 打断铁律）。
 */
void voice_capture_interrupt(void);

/** 录音是否进行中（含已受理未开录的短暂窗口）。 */
bool voice_capture_busy(void);

/** 已录时长（毫秒），供浮层倒计时。 */
uint32_t voice_capture_elapsed_ms(void);

/** 当前音量档 0~5，供浮层波形。 */
uint8_t voice_capture_level(void);

/**
 * 释放 PCM 缓冲（网络任务派发完必须调用）。
 *
 * PCM 所有权随请求转移：录音模块交出缓冲后不再持有，
 * 网络任务在 net_dispatch() 返回后调它释放，否则 PSRAM 泄漏。
 */
void voice_capture_free_pcm(const void *pcm);

/**
 * 音频任务每轮调一次：驱动采集与收尾（Core 0 调用）。
 *
 * 录音期间音频任务**不消费**播放消息队列（半双工纪律）：
 * ICS43434 + MAX98357 共用 I2S，换向会打断录音。
 */
void voice_capture_audio_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* VOICE_CAPTURE_H */
