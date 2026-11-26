/**
 * esp_heap_caps 主机侧 stub —— 只为让 host gcc 语法检查通过，签名对齐 ESP-IDF。
 * 真实实现把 PSRAM 分配交给 IDF 的堆管理器；本 stub 只是声明。
 */
#ifndef ABBOT_STUB_ESP_HEAP_CAPS_H
#define ABBOT_STUB_ESP_HEAP_CAPS_H

#include <cstddef>
#include <cstdint>

#define MALLOC_CAP_SPIRAM (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)

void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
/* 剩余可用字节数 —— 录音前要确认 PSRAM 够放 320KB 缓冲（doc/03 ERR_PSRAM_OOM）。 */
size_t heap_caps_get_free_size(uint32_t caps);

#endif /* ABBOT_STUB_ESP_HEAP_CAPS_H */
