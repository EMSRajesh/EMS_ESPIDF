#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ESP-IDF FreeRTOS reports uxTaskGetStackHighWaterMark() in bytes. */
typedef struct {
    const char *name;
    uint32_t allocated_bytes;
    uint32_t minimum_free_bytes;
    TickType_t last_report_tick;
    bool warning_reported;
    bool critical_reported;
} ems_stack_diag_t;

static inline void ems_stack_diag_init(ems_stack_diag_t *d,
                                      const char *name,
                                      uint32_t allocated_bytes)
{
    if (d == NULL) return;
    d->name = name;
    d->allocated_bytes = allocated_bytes;
    d->minimum_free_bytes = UINT32_MAX;
    d->last_report_tick = 0;
    d->warning_reported = false;
    d->critical_reported = false;
}

static inline void ems_stack_diag_sample(ems_stack_diag_t *d,
                                         const char *tag)
{
    if (d == NULL || d->allocated_bytes == 0U) return;
    const uint32_t free_bytes = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    if (free_bytes < d->minimum_free_bytes) d->minimum_free_bytes = free_bytes;

    const uint32_t warning_limit = (d->allocated_bytes / 100U) * 15U > 512U
        ? (d->allocated_bytes / 100U) * 15U : 512U;
    const uint32_t critical_limit = (d->allocated_bytes / 100U) * 5U > 256U
        ? (d->allocated_bytes / 100U) * 5U : 256U;

    if (free_bytes <= critical_limit && !d->critical_reported) {
        ESP_LOGE(tag, "STACK CRITICAL task=%s allocated=%luB free=%luB min_free=%luB",
                 d->name, (unsigned long)d->allocated_bytes,
                 (unsigned long)free_bytes, (unsigned long)d->minimum_free_bytes);
        d->critical_reported = true;
    } else if (free_bytes <= warning_limit && !d->warning_reported) {
        ESP_LOGW(tag, "STACK LOW task=%s allocated=%luB free=%luB min_free=%luB",
                 d->name, (unsigned long)d->allocated_bytes,
                 (unsigned long)free_bytes, (unsigned long)d->minimum_free_bytes);
        d->warning_reported = true;
    }

    const TickType_t now = xTaskGetTickCount();
    if (d->last_report_tick == 0U ||
        (TickType_t)(now - d->last_report_tick) >= pdMS_TO_TICKS(10000U)) {
        ESP_LOGI(tag, "STACK task=%s allocated=%luB current_free=%luB min_ever_free=%luB",
                 d->name, (unsigned long)d->allocated_bytes,
                 (unsigned long)free_bytes, (unsigned long)d->minimum_free_bytes);
        d->last_report_tick = now;
    }
}
