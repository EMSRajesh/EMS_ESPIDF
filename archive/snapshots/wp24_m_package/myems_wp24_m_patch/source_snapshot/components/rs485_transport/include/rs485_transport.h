#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS485_MAX_TX_BYTES       256U
#define RS485_MAX_RX_CHUNK       128U
#define RS485_DEFAULT_EVENT_Q    20U
#define RS485_DEFAULT_TX_Q       4U
#define RS485_DEFAULT_APP_EVENT_Q 16U

typedef struct rs485_transport *rs485_handle_t;

typedef struct {
    uart_port_t uart_num;
    int tx_gpio;
    int rx_gpio;
    int rts_gpio;             /* Board direction-enable pin, if RTS mode is used. */
    uint32_t baud_rate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    size_t rx_ring_buffer_size;
    size_t tx_ring_buffer_size;
    int uart_event_queue_length;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    bool use_rts_half_duplex;
    uint32_t recovery_bus_idle_ms; /* Minimum RX-silence interval during recovery. */
    uint32_t recovery_timeout_ms;  /* Maximum duration before recovery fails closed. */
} rs485_config_t;

typedef enum {
    RS485_EVT_RX_DATA = 0,
    RS485_EVT_RX_IDLE,
    RS485_EVT_TX_DONE,
    RS485_EVT_TX_ERROR,
    RS485_EVT_RX_ERROR,
    RS485_EVT_RX_LOSS
} rs485_event_type_t;

typedef enum {
    RS485_ERR_NONE = 0,
    RS485_ERR_UART_FRAME,
    RS485_ERR_UART_PARITY,
    RS485_ERR_UART_BREAK,
    RS485_ERR_FIFO_OVERFLOW,
    RS485_ERR_RING_BUFFER_FULL,
    RS485_ERR_EVENT_QUEUE_FULL,
    RS485_ERR_TX_TIMEOUT,
    RS485_ERR_TX_DRIVER
} rs485_error_t;

typedef struct {
    rs485_event_type_t type;
    uint32_t tx_id;           /* Valid for TX events; zero for RX events. */
    int64_t timestamp_us;     /* Timestamp taken when the task handled the event. */
    size_t length;
    rs485_error_t error;
    uint8_t data[RS485_MAX_RX_CHUNK]; /* Copied payload for RX_DATA only. */
} rs485_event_t;

typedef enum {
    RS485_RECOVERY_IDLE = 0,
    RS485_RECOVERY_REQUESTED,
    RS485_RECOVERY_ACTIVE,
    RS485_RECOVERY_COMPLETE,
    RS485_RECOVERY_FAILED
} rs485_recovery_state_t;

typedef struct {
    uint32_t tx_submitted;
    uint32_t tx_completed;
    uint32_t tx_failed;
    uint32_t rx_bytes;
    uint32_t rx_chunks;
    uint32_t uart_frame_errors;
    uint32_t uart_parity_errors;
    uint32_t uart_overflows;
    uint32_t app_event_drops;
    uint32_t recovery_requests;
    uint32_t recovery_count;
    uint32_t recovery_bytes_discarded;
    uint32_t recovery_events_discarded;
    uint32_t recovery_success_count;
    uint32_t recovery_failure_count;
    uint32_t recovery_timeout_count;
    uint32_t recovery_duration_max_us;
} rs485_stats_t;

/* Default config uses the Waveshare board's documented RS-485 pin mapping. */
void rs485_config_default(rs485_config_t *config);

/* Opens one physical bus and starts its transport task. */
esp_err_t rs485_open(const rs485_config_t *config, rs485_handle_t *out_handle);

/* Copies data into transport-owned storage. Success means accepted, not yet sent. */
esp_err_t rs485_submit_tx(rs485_handle_t handle,
                          const uint8_t *data,
                          size_t length,
                          uint32_t tx_id);

/* Request asynchronous recovery. The transport task owns all UART/queue cleanup.
 * Returns ESP_ERR_INVALID_STATE while TX dispatch/submission is in progress;
 * caller should retry after the active TX operation has completed. */
esp_err_t rs485_recovery_begin(rs485_handle_t handle);

/* Read recovery state. COMPLETE remains latched until rs485_recovery_release(). */
esp_err_t rs485_recovery_get_state(rs485_handle_t handle,
                                   rs485_recovery_state_t *state);

/* Request release of a completed recovery. Poll state until IDLE before TX. */
esp_err_t rs485_recovery_release(rs485_handle_t handle);

/* Receive a copied transport event. timeout_ticks is a FreeRTOS tick count. */
esp_err_t rs485_get_event(rs485_handle_t handle,
                          rs485_event_t *event,
                          TickType_t timeout_ticks);

/* Copies diagnostic counters to the caller. */
esp_err_t rs485_get_stats(rs485_handle_t handle, rs485_stats_t *stats);

#if CONFIG_MYEMS_WP24_FAULT_INJECTION
/* Test-only hooks. Not compiled into production builds when the Kconfig option is off. */
esp_err_t rs485_test_inject_rx_loss(rs485_handle_t handle, rs485_error_t error);
esp_err_t rs485_test_force_recovery_activity(rs485_handle_t handle, bool enable);
#endif

/* Stops and destroys the transport. Call only after the Modbus master is stopped. */
esp_err_t rs485_close(rs485_handle_t handle);

#ifdef __cplusplus
}
#endif
