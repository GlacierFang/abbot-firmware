// voice_capture.cpp —— 设备端语音录音生产者（doc/03 §3.1 录音浮层 / doc/04 §3.5 媒体铁律）
//
// 职责边界：
//   本模块只做「录音 → 落盘 → 投递 NET_OP_VOICE」。
//   离线补传、队列策略、淘汰规则全在 outbox 层，本模块不碰。
//   服务端零改动：仍复用 /api/v1/study/voice。
//
// 线程模型（doc/02 §4.1 双核铁律）：
//   UI 核（Core 1）只设置意图标志（start/stop/cancel/interrupt），**不做 TF 卡 IO、不碰 I2S**。
//   音频任务（Core 0）每轮调 voice_capture_audio_poll()，采集、落盘、投递全在 Core 0 完成。
//   这样 Core 1 永远不会阻塞在 I2S 或 SD 上。

#include "voice_capture.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdio.h>
#include <string.h>

#include "hal_interfaces.h"
#include "hal_shared.h"
#include "net_api.h"

namespace {

// 采样参数：16kHz / 16bit / Mono（doc/04 §3.5 语音契约）
constexpr size_t kMaxSamples = VOICE_MAX_SAMPLES;
constexpr size_t kChunkSamples = 1024;

enum State : uint8_t {
    kIdle = 0,
    kPending,  /* 已受理，等 Core 0 真正开录 */
    kRecording,
};

volatile uint8_t s_state = kIdle;
volatile uint32_t s_elapsed_ms = 0;
volatile uint8_t s_level = 0; /* 0~5 音量档，供浮层波形 */

uint32_t s_start_ms = 0;
int16_t *s_buf = nullptr;    /* PSRAM 暂存缓冲 */
size_t s_samples = 0;        /* 已采到的采样点数 */
voice_context_t s_ctx = {};
/* UI 核设置意图标志，Core 0 消费 */
volatile bool s_req_start = false;
volatile bool s_req_stop = false;
volatile bool s_req_cancel = false;
volatile bool s_req_interrupt = false;

/* 叫醒音频任务的敲门砖 —— 用静态存储，别在 UI 任务栈上摆 700 字节。 */
static audio_msg_t s_wake_msg = {};

void toast(const char *text)
{
    ui_command_t cmd = {};
    cmd.kind = 1;
    snprintf(cmd.text, sizeof(cmd.text), "%s", text);
    xQueueSend(hal_ui_command_queue(), &cmd, 0);
}

/** 峰值音量 → 0~5 档（浮层波形用）。 */
uint8_t calc_level(const int16_t *pcm, size_t n)
{
    int32_t peak = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = pcm[i] < 0 ? -(int32_t)pcm[i] : (int32_t)pcm[i];
        if (v > peak) {
            peak = v;
        }
    }
    return voice_level_of(peak);
}

/** 录音落盘 → 投递网络请求（正常提交路径）。 */
void submit_recording(void)
{
    char path[72];
    snprintf(path, sizeof(path), "/outbox/task_%u.pcm", (unsigned)millis());

    bool saved = hal_storage_write_binary(path, (const uint8_t *)s_buf,
                                          (uint32_t)voice_pcm_bytes(s_samples)) == HAL_OK;
    if (!saved) {
        /* 落盘失败 = 这段录音留不下来，直接丢弃，别让用户以为已经存好了。 */
        toast("录音没能存下来，请再试一次。");
        s_samples = 0;
        s_state = kIdle;
        return;
    }

    net_request_t req = {};
    req.op = NET_OP_VOICE;
    req.pcm = s_buf;
    req.pcm_bytes = (uint32_t)voice_pcm_bytes(s_samples);
    snprintf(req.context_page, sizeof(req.context_page), "%s", s_ctx.page);
    snprintf(req.pack_id, sizeof(req.pack_id), "%s", s_ctx.pack_id);
    snprintf(req.card_id, sizeof(req.card_id), "%s", s_ctx.card_id);
    snprintf(req.media_path, sizeof(req.media_path), "%s", path);

    if (hal_net_request_queue() == NULL ||
        xQueueSend(hal_net_request_queue(), &req, 0) != pdTRUE) {
        /* 网络队列都满了 —— 录音已经在卡上，入队等以后补传（doc/04 §3.5）。 */
        net_outbox_enqueue_voice("", path, s_ctx.page, s_ctx.pack_id, s_ctx.card_id);
        toast("已自动存入待问收件箱（联网后自动发送）。");
        heap_caps_free(s_buf);
        s_buf = nullptr;
        s_samples = 0;
        s_state = kIdle;
        return;
    }

    /* 所有权随请求转移：网络任务派发完调 voice_capture_free_pcm() 释放。
     * 这里把 s_buf 置空 —— 下一段录音另开一块缓冲，
     * 用户不用等上一次网络往返结束就能接着说。 */
    s_buf = nullptr;
    s_samples = 0;
    s_state = kIdle;
}

/** 录音落盘 → 直接进待问队列（提醒打断铁律，doc/03 §3.1）。 */
void save_to_outbox(const char *tip)
{
    char path[72];
    snprintf(path, sizeof(path), "/outbox/task_%u.pcm", (unsigned)millis());
    bool saved = hal_storage_write_binary(path, (const uint8_t *)s_buf,
                                          (uint32_t)voice_pcm_bytes(s_samples)) == HAL_OK;
    if (saved) {
        if (net_outbox_enqueue_voice("", path, s_ctx.page, s_ctx.pack_id, s_ctx.card_id)) {
            toast(tip);
        }
    }
    s_samples = 0;
    s_state = kIdle;
}

/** 取消：不落盘、不入队。 */
void discard_recording(void)
{
    s_samples = 0;
    s_level = 0;
    s_elapsed_ms = 0;
    s_state = kIdle;
}

}  // namespace

extern "C" {

bool voice_capture_busy(void)
{
    return s_state != kIdle;
}

uint32_t voice_capture_elapsed_ms(void)
{
    return s_elapsed_ms;
}

uint8_t voice_capture_level(void)
{
    return s_level;
}

bool voice_capture_start(const voice_context_t *ctx)
{
    if (s_state != kIdle) {
        return false;
    }
    if (s_buf == nullptr) {
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < voice_psram_need()) {
            toast("内存空间紧张，请稍后再试。");
            return false;
        }
        s_buf = (int16_t *)heap_caps_malloc(voice_pcm_bytes(kMaxSamples), MALLOC_CAP_SPIRAM);
        if (s_buf == nullptr) {
            toast("内存空间紧张，请稍后再试。");
            return false;
        }
    }
    if (ctx != nullptr) {
        s_ctx = *ctx;
    } else {
        memset(&s_ctx, 0, sizeof(s_ctx));
    }
    s_samples = 0;
    s_elapsed_ms = 0;
    s_level = 0;
    s_req_start = true;
    s_req_stop = false;
    s_req_cancel = false;
    s_req_interrupt = false;
    s_state = kPending;

    /*
     * 叫醒音频任务：它阻塞在播放队列上（省电），不会自己醒。
     * 敲门用 op=2（开始录音）—— 真正的开录动作在 voice_capture_audio_poll() 里。
     */
    s_wake_msg.op = 2;
    if (hal_audio_pcm_queue() == NULL ||
        xQueueSend(hal_audio_pcm_queue(), &s_wake_msg, 0) != pdTRUE) {
        s_req_start = false;
        s_state = kIdle;
        toast("麦克风正忙，请稍后再试。");
        return false;
    }
    return true;
}

void voice_capture_stop(void)
{
    if (s_state == kRecording || s_state == kPending) {
        s_req_stop = true;
    }
}

void voice_capture_cancel(void)
{
    if (s_state == kRecording || s_state == kPending) {
        s_req_cancel = true;
    }
}

void voice_capture_interrupt(void)
{
    if (s_state == kRecording || s_state == kPending) {
        s_req_interrupt = true;
    }
}

void voice_capture_free_pcm(const void *pcm)
{
    if (pcm == nullptr) {
        return;
    }
    heap_caps_free((int16_t *)pcm);
}

/**
 * 音频任务每轮调一次：驱动录音采集与收尾。
 *
 * 录音期间音频任务**不消费**播放消息队列（半双工纪律）：
 * ICS43434 + MAX98357 共用 I2S 驱动，换向会打断正在进行的录音。
 */
void voice_capture_audio_poll(void)
{
    /* 取消优先于开录：用户可能在真正开录前就反悔了。 */
    if (s_req_cancel) {
        s_req_cancel = false;
        if (s_state == kRecording) {
            hal_audio_stop_record();
        }
        discard_recording();
        return;
    }

    /* 还没开录就要结束 —— 什么都不用做。 */
    if (s_req_stop && s_state == kPending) {
        s_req_stop = false;
        discard_recording();
        return;
    }

    if (s_req_start) {
        s_req_start = false;
        hal_status_t st = hal_audio_start_record();
        if (st != HAL_OK) {
            toast(st == HAL_ERR_NO_SPACE ? "内存空间紧张，请稍后再试。"
                                         : "麦克风没能打开，请再试一次。");
            discard_recording();
            return;
        }
        s_start_ms = millis();
        s_elapsed_ms = 0;
        s_samples = 0;
        s_state = kRecording;
    }

    if (s_state == kRecording) {
        size_t room = kMaxSamples - s_samples;
        if (room > 0) {
            size_t want = (room < kChunkSamples) ? room : kChunkSamples;
            int got = hal_audio_read_chunk(s_buf + s_samples, want);
            if (got > 0) {
                s_samples += (size_t)got;
                s_level = calc_level(s_buf + s_samples - (size_t)got, (size_t)got);
            }
        }
        s_elapsed_ms = millis() - s_start_ms;
    }

    if (s_req_interrupt) {
        s_req_interrupt = false;
        if (s_state == kRecording) {
            hal_audio_stop_record();
        }
        /* 有效时长不够 1.0 秒 → 直接丢弃，不入队（doc/03 §3.1 打断铁律）。 */
        if (s_samples >= VOICE_INTERRUPT_MIN_SAMPLES) {
            save_to_outbox("录音已自动存入待问队列");
        } else {
            discard_recording();
        }
        return;
    }

    if (s_req_stop || (s_state == kRecording && s_samples >= kMaxSamples)) {
        s_req_stop = false;
        if (s_state == kRecording) {
            hal_audio_stop_record();
        }
        /* 太短 —— 说清没听清，别让用户白等一次网络往返（doc/03 ERR_ASR_4003）。 */
        if (!voice_samples_enough(s_samples)) {
            toast("没有听清刚才的话，请靠近麦克风再说一次。");
            discard_recording();
            return;
        }
        submit_recording();
        return;
    }
}

}  // extern "C"
