/**
 * hal_interfaces.h —— HAL 层唯一对外契约
 *
 * 逐字抄自 abbot/doc/02-硬件抽象层与底层驱动契约.md §六。
 * 任何模块都只能经下列签名访问 HAL, 严禁直接调用 Wire / SD_MMC /
 * i2s_driver_* / esp_camera_* 等底层 API
 * (唯一例外: hal_storage 与 hal_audio 的实现文件自身)。
 *
 * HAL 层只返回 hal_status_t 语义码, 严禁在 HAL 内拼装面向用户的中文文案;
 * 所有面向用户的文案一律经 doc/03 第四节的 ui_show_friendly_error(ErrorCode) 查表渲染。
 */
#pragma once
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- 通用返回码 ---------- */
typedef enum {
    HAL_OK              = 0,
    HAL_ERR_INVALID     = -1,   // 参数非法
    HAL_ERR_TIMEOUT     = -2,   // 超时（含 AF 对焦熔断）
    HAL_ERR_IO          = -3,   // 底层总线 / SD 读写失败
    HAL_ERR_NOT_FOUND   = -4,   // 文件或目标不存在
    HAL_ERR_NO_SPACE    = -5,   // 空间不足（原子写入失败时保留原文件）
    HAL_ERR_UNSUPPORTED = -6,   // 该设备/状态不支持（如无 TF 卡）
} hal_status_t;

/* ---------- hal_storage：TF 卡存储（SD_MMC 1-Bit） ---------- */
hal_status_t hal_storage_init_and_scaffold(void);                 // 挂载 + 空白卡自愈建全套目录
hal_status_t hal_storage_get_file_size(const char *path, uint32_t *out_size);
hal_status_t hal_storage_read_range(const char *path, uint32_t offset,
                                    uint8_t *buf, uint32_t len, uint32_t *out_read);
hal_status_t hal_storage_read_text(const char *path, char *buf, uint32_t max_len, uint32_t *out_len);
hal_status_t hal_storage_atomic_write_text(const char *path, const char *text, uint32_t len);
hal_status_t hal_storage_list_files(const char *dir_path, void (*cb)(const char *name, void *ctx), void *ctx);
hal_status_t hal_storage_delete_file(const char *path);
hal_status_t hal_storage_get_dir_usage(const char *dir_path, uint64_t *out_bytes);
hal_status_t hal_storage_get_file_mtime(const char *path, uint32_t *out_epoch);
bool        hal_storage_is_mounted(void);

/* ---------- hal_display：ST7796S 竖屏 + 背光 PWM ---------- */
hal_status_t hal_display_init(void);
hal_status_t hal_display_flush(void);
hal_status_t hal_display_set_backlight(uint8_t level);             // 0~5 档
uint8_t     hal_display_get_backlight(void);
/**
 * 背光从当前 PWM 占空比**平滑淡出**到 0（关机流程用）。
 * steps 为渐暗步数，每步延时 5ms（例如 30 步 ≈ 150ms）。
 */
hal_status_t hal_display_fade_out_backlight(uint16_t steps);

/* ---------- hal_camera：OV5640 拍题 ---------- */
hal_status_t hal_camera_init(void);
hal_status_t hal_camera_start_preview(void);                        // FRAMESIZE_HVGA
hal_status_t hal_camera_capture_jpeg(uint8_t **out_jpg, size_t *out_len);  // 切 UXGA 抓拍，含 1500ms AF 熔断
hal_status_t hal_camera_stop_preview(void);

/* ---------- hal_photo：拍题抓拍请求（UI 核 → 相机任务） ---------- */
/**
 * 提交一次抓拍请求（投递到 `q_photo_request`，doc/02 §4.2 队列表已定的方向）。
 * `session_id` 是本题会话号（doc/05 §5.1：同题追问沿用同一个），可传 NULL。
 * `pack_id` / `card_id` 是当前卡片上下文（doc/05 §5.1），可传 NULL 或空串。
 * 队列满时返回 `HAL_ERR_NO_SPACE`，由 UI 层给出温和中文提示。
 */
hal_status_t hal_photo_request_submit(const char *session_id, uint8_t want_uxga, uint8_t with_voice,
                                      const char *pack_id, const char *card_id);

/* ---------- hal_audio：ICS43434 + MAX98357 半双工 I2S ---------- */
hal_status_t hal_audio_init(void);
hal_status_t hal_audio_start_record(void);                         // 16kHz/16bit/Mono，丢弃首 30ms
hal_status_t hal_audio_stop_record(void);
hal_status_t hal_audio_play_wav_file(const char *path);            // 校验 RIFF 头，跳过 44 字节
hal_status_t hal_audio_set_volume(uint8_t level);                  // 1~5 档
/**
 * 停止当前播放（TTS 流式 / WAV 文件）并让功放静音。
 *
 * **跨核安全**：只在 Core 1 置标志位，真正的中断由 Core 0 的音频任务观察 ——
 * 本函数严禁在调用方直接 uninstall I2S 驱动（会和 Core 0 抢 I2S_NUM_0）。
 * 关机流程用它保证「正在关机…」这段不再出声。
 */
hal_status_t hal_audio_stop_playback(void);
/**
 * 设备端合成提示音（不依赖 LittleFS / ding.wav 资产）。
 * 在功放上直接推出 freq_hz 的正弦音，时长 duration_ms 毫秒。
 * 音量沿用 hal_audio_set_volume 的 1~5 档。
 * 必须在音频任务里调用（本函数是阻塞的，会占住 I2S 播放通道）。
 */
hal_status_t hal_audio_play_tone(uint16_t freq_hz, uint16_t duration_ms);

/**
 * 投递一次提示音请求（UI 核 → 音频任务，**不阻塞 UI**）。
 * 音频任务收到后调 hal_audio_play_tone() 真正出声。
 * 队列满时返回 `HAL_ERR_NO_SPACE`，由 UI 层给出温和中文提示。
 */
hal_status_t hal_audio_request_tone(uint16_t freq_hz, uint16_t duration_ms);

/* ---------- hal_joystick：双轴摇杆五向事件 ---------- */
hal_status_t hal_joystick_init(void);                              // 开机 300ms 中点校准
hal_status_t hal_joystick_poll(void);                              // 50Hz 调用，完成消抖与归类后投递队列

/* ---------- hal_power：MAX17043 电量计与低功耗 ---------- */
hal_status_t hal_power_init(void);
hal_status_t hal_power_read_battery(float *out_volts, uint8_t *out_soc);  // 持 g_i2c_bus_mutex
hal_status_t hal_power_enter_light_sleep(void);                    // GPIO0 / RTC 闹钟唤醒
hal_status_t hal_power_enter_deep_sleep(void);

/* ---------- 休眠唤醒后的外设恢复（02 第 3.6 节强制） ---------- */
hal_status_t hal_recover_after_sleep(void);                        // 重建 SPI 屏 / I2C / I2S 状态

#ifdef __cplusplus
}
#endif
