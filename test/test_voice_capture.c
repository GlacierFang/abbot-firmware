/**
 * test_voice_capture.c —— 语音录音生产者的纯逻辑测试（主机侧）
 *
 * 覆盖 voice_capture.h 里不依赖硬件的部分：PCM 字节数、有效时长下限、
 * 音量分档、PSRAM 需求。采集与落盘是硬件路径，靠 CI 的固件编译兜底。
 */
#include <stdio.h>
#include <stdlib.h>

#include "voice_capture.h"

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                               \
    do {                                                          \
        g_checks++;                                               \
        if (!(cond)) {                                            \
            g_failures++;                                         \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                         \
    } while (0)

int main(void)
{
    /* ---- PCM 字节数：16bit/Mono 裸 PCM = 采样点数 × 2 ---- */
    CHECK(voice_pcm_bytes(0) == 0);
    CHECK(voice_pcm_bytes(16000) == 32000);           /* 1 秒 = 32KB */
    CHECK(voice_pcm_bytes(VOICE_MAX_SAMPLES) == 320000); /* 10 秒 = 320KB */

    /* ---- 有效时长下限 0.5 秒（与云端 ASR 同口径） ---- */
    CHECK(!voice_samples_enough(0));
    CHECK(!voice_samples_enough(7999));
    CHECK(voice_samples_enough(8000));
    CHECK(voice_samples_enough(VOICE_MAX_SAMPLES));

    /* ---- PSRAM 需求：320KB 缓冲 + 32KB 余量（doc/03 ERR_PSRAM_OOM） ---- */
    CHECK(voice_psram_need() == 320000u + 32u * 1024u);
    CHECK(voice_psram_need() > voice_pcm_bytes(VOICE_MAX_SAMPLES));

    /* ---- 音量分档 0~5（浮层波形用） ---- */
    CHECK(voice_level_of(0) == 0);
    CHECK(voice_level_of(199) == 0);
    CHECK(voice_level_of(200) == 1);
    CHECK(voice_level_of(1499) == 1);
    CHECK(voice_level_of(1500) == 2);
    CHECK(voice_level_of(3999) == 2);
    CHECK(voice_level_of(4000) == 3);
    CHECK(voice_level_of(8999) == 3);
    CHECK(voice_level_of(9000) == 4);
    CHECK(voice_level_of(15999) == 4);
    CHECK(voice_level_of(16000) == 5);
    CHECK(voice_level_of(32767) == 5);

    /* ---- 采样参数与契约一致：16kHz / 16bit / Mono，最长 10 秒 ---- */
    CHECK(VOICE_SAMPLE_RATE == 16000);
    CHECK(VOICE_MAX_SECONDS == 10);
    CHECK(VOICE_MAX_SAMPLES == 160000);
    CHECK(VOICE_MIN_SAMPLES == 8000);
    CHECK(VOICE_INTERRUPT_MIN_SAMPLES == 16000);

    printf("voice_capture: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
