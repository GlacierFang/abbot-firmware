/**
 * semphr 主机侧 stub —— 只为让 host gcc 语法检查通过，签名对齐真机。
 */
#ifndef ABBOT_STUB_SEMPHR_H
#define ABBOT_STUB_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
void xSemaphoreDelete(SemaphoreHandle_t s);

#endif /* ABBOT_STUB_SEMPHR_H */
