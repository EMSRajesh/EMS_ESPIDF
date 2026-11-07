#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "rs485_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_MAX_REGISTERS 125U
#define MODBUS_MAX_COILS 2000U
#define MODBUS_MAX_WRITE_REGISTERS 123U
/* FC23 request ADU is limited to 256 bytes; write quantity is therefore 121 max. */
#define MODBUS_MAX_FC23_WRITE_REGISTERS 121U
#define MODBUS_MAX_WRITE_COILS 1968U
#define MODBUS_MAX_ADU_BYTES 256U
#define MODBUS_BIT_BYTES ((MODBUS_MAX_COILS + 7U) / 8U)
#define MODBUS_RETRIES_USE_DEFAULT 0xFFU
#define MODBUS_MAX_RETRIES 5U

typedef struct modbus_master *modbus_master_handle_t;

typedef enum {
    MODBUS_MASTER_STOPPED = 0,
    MODBUS_MASTER_STARTING,
    MODBUS_MASTER_RUNNING,
    MODBUS_MASTER_QUIESCING,
    MODBUS_MASTER_DRAINING,
    MODBUS_MASTER_STOPPING,
    MODBUS_MASTER_FAILED
} modbus_master_lifecycle_state_t;
typedef enum {
    MODBUS_RESULT_OK = 0, MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_CRC_ERROR,
    MODBUS_RESULT_PROTOCOL_ERROR, MODBUS_RESULT_EXCEPTION,
    MODBUS_RESULT_TRANSPORT_ERROR, MODBUS_RESULT_CANCELLED
} modbus_result_status_t;

/*
 * Common supported services:
 * FC01/02 read bits, FC03/04 read registers, FC05/06 single writes,
 * FC15/16 multiple writes, FC23 read/write registers.
 * Read start/count use start_register/register_count. For FC23, use
 * read_start_register/read_register_count and start_register/register_count
 * as the write range. Register write values go in write_registers[].
 * Coil writes use packed LSB-first write_coils[].
 */
typedef struct {
    uint32_t request_id;
    uint8_t slave_addr;       /* 1..247; broadcast is intentionally not supported */
    uint8_t function_code;
    uint16_t start_register;
    uint16_t register_count;
    uint16_t read_start_register; /* FC23 read range */
    uint16_t read_register_count; /* FC23 read count */
    uint32_t timeout_ms;
    /* 0 means no retry; MODBUS_RETRIES_USE_DEFAULT selects config default. */
    uint8_t max_retries;
    /* Writes are not retried unless the caller explicitly confirms idempotency. */
    bool allow_write_retries;
    uint16_t write_registers[MODBUS_MAX_WRITE_REGISTERS];
    uint8_t write_coils[(MODBUS_MAX_WRITE_COILS + 7U) / 8U];
} modbus_request_t;

typedef struct {
    uint32_t request_id;
    uint8_t slave_addr;
    uint8_t function_code;
    modbus_result_status_t status;
    uint8_t exception_code;
    uint8_t retry_count;
    uint16_t register_count;
    uint16_t registers[MODBUS_MAX_REGISTERS];
    uint16_t bit_count;
    uint8_t bits[MODBUS_BIT_BYTES]; /* packed LSB-first for FC01/02 */
} modbus_result_t;

typedef struct {
    rs485_handle_t transport;
    UBaseType_t task_priority;
    uint32_t task_stack_size;
    uint32_t default_timeout_ms;
    uint8_t default_retries;
    uint16_t request_queue_length;
    uint16_t completion_queue_length;
} modbus_master_config_t;

typedef struct {
    uint32_t requests_accepted, requests_completed, timeouts, crc_errors;
    uint32_t protocol_errors, transport_errors, completion_drops;
    uint32_t completion_backpressure_rejections;
    uint32_t requests_cancelled;
    uint32_t active_requests_cancelled;
    uint32_t queued_requests_cancelled;
    uint32_t unexpected_slave_frames;
} modbus_master_stats_t;

void modbus_master_config_default(modbus_master_config_t *config, rs485_handle_t transport);
esp_err_t modbus_master_start(const modbus_master_config_t *config, modbus_master_handle_t *out_handle);
esp_err_t modbus_master_submit(modbus_master_handle_t handle, const modbus_request_t *request);
/*
 * Cancel the active request and all requests currently queued, publishing one
 * MODBUS_RESULT_CANCELLED completion for each. Callers must quiesce submissions
 * before invoking this API. The master task remains running. WP2.5-B will
 * provide atomic API-admission closure for shutdown.
 */
esp_err_t modbus_master_cancel_pending(modbus_master_handle_t handle,
                                       TickType_t timeout_ticks,
                                       uint32_t *cancelled_count);
esp_err_t modbus_master_get_completion(modbus_master_handle_t handle, modbus_result_t *result, TickType_t timeout_ticks);
esp_err_t modbus_master_get_stats(modbus_master_handle_t handle, modbus_master_stats_t *stats);
esp_err_t modbus_master_get_lifecycle_state(modbus_master_handle_t handle, modbus_master_lifecycle_state_t *state);
/*
 * WP2.5-B Option A, phase 1: close request admission, stop the producer task,
 * publish cancellation completions for accepted work, and leave the completion
 * queue available for draining. Resource deinitialization is a later phase.
 * Until WP2.5-B3/B4 adds deinit, do not attempt to restart this singleton after
 * stop; the handle remains valid in MODBUS_MASTER_DRAINING.
 */
esp_err_t modbus_master_stop(modbus_master_handle_t handle);

#ifdef __cplusplus
}
#endif
