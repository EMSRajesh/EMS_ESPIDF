#include <inttypes.h>
#include <stdio.h>

#include "ems_acquisition.h"
#include "ems_config_gen.h"
#include "ems_event.h"
#include "ems_poll_scheduler.h"
#include "ems_profile.h"
#include "ems_sched_core.h"
#include "ems_stack_diag.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modbus_rtu_master.h"
#include "modbus_self_test.h"
#include "rs485_transport.h"

static const char *TAG = "myems_app";


#if CONFIG_MYEMS_MODBUS_SELF_TEST
#define MYEMS_SELF_TEST_TASK_STACK_BYTES 8192U
#define MYEMS_SELF_TEST_TASK_PRIORITY    5U

typedef struct {
    modbus_master_handle_t master;
    rs485_handle_t transport;
    TaskHandle_t waiter;
} myems_self_test_task_args_t;

static esp_err_t s_self_test_result = ESP_FAIL;

static void myems_self_test_task(void *arg)
{
    const myems_self_test_task_args_t *args =
        (const myems_self_test_task_args_t *)arg;

    s_self_test_result = myems_modbus_self_test_run(args->master, args->transport);
    ESP_LOGI(TAG, "Self-test execution finished; collecting stack watermark");

    ems_stack_diag_t stack_diag;
    ems_stack_diag_init(&stack_diag, "myems_self_test",
                        MYEMS_SELF_TEST_TASK_STACK_BYTES);
    ems_stack_diag_sample(&stack_diag, TAG);

    xTaskNotifyGive(args->waiter);
    vTaskDelete(NULL);
}
#endif


#if !CONFIG_MYEMS_MODBUS_SELF_TEST
/* --------------------------------------------------------------------------
 * DP-WP4 CONFIGURATION
 * --------------------------------------------------------------------------
 * Devices, poll groups and bus settings come from the generated tables in
 * main/generated/ (tools/profile/myems_profile_tool.py). The 09_057 demo
 * meter profiles are gone. Before anything is polled the tables are
 * re-checked (ems_profile_activate: CRC, target/classification V-13, limits
 * V-15, structural rules) and the scheduler plan is built (Design 5.3 step 4).
 * On any failure: EVT_CONFIG_REJECTED, error log, nothing is polled (DR-03).
 */
#if CONFIG_MYEMS_CONFIG_TARGET_SIM
#define MYEMS_CONFIG_TARGET_NAME "sim"
#define MYEMS_CONFIG_TARGET_VALUE EMS_TARGET_SIM
#define MYEMS_VALUE_DUMP_MS 5000U     /* HIL runner parses the VAL lines */
#else
#define MYEMS_CONFIG_TARGET_NAME "prod"
#define MYEMS_CONFIG_TARGET_VALUE EMS_TARGET_PROD
#define MYEMS_VALUE_DUMP_MS 60000U
#endif

static ems_sched_plan_t s_plan;
#if CONFIG_MYEMS_CONFIG_TARGET_SIM
static const bool s_sim_build = true;
#else
static const bool s_sim_build = false;
#endif

static portMUX_TYPE s_event_mux = portMUX_INITIALIZER_UNLOCKED;
static void event_lock(void *ctx)
{
    (void)ctx;
    taskENTER_CRITICAL(&s_event_mux);
}
static void event_unlock(void *ctx)
{
    (void)ctx;
    taskEXIT_CRITICAL(&s_event_mux);
}

static void myems_config_rejected(const char *rule, const char *reason, uint32_t data)
{
    (void)ems_event_emit(EMS_EVT_CONFIG_REJECTED, EMS_EVENT_NO_SLOT, EMS_EVENT_NO_PARAM, data,
                         esp_timer_get_time());
    ESP_LOGE(TAG, "EVT_CONFIG_REJECTED rule=%s: %s", rule, reason);
}

static bool myems_config_accept(const ems_config_build_t *cfg)
{
    ESP_LOGI(TAG, "CONFIG site=%s target=%s classification=%s crc=0x%08" PRIX32 " devices=%u",
             cfg->site_id, cfg->target == EMS_TARGET_SIM ? "sim" : "prod",
             ems_classification_name(cfg->classification), cfg->crc32, (unsigned)cfg->device_count);
    ESP_LOGI(TAG, "CONFIG inputs: %s (sha256 %s, tool %s, umd %s)",
             cfg->inputs_summary, cfg->inputs_sha256, cfg->tool_version, cfg->umd_version);

    ems_activation_report_t rep;
    const ems_activation_status_t st = ems_profile_activate(cfg, s_sim_build, &rep);
    if (st != EMS_ACT_OK) {
        char reason[160];
        if (st == EMS_ACT_CRC) {
            (void)snprintf(reason, sizeof(reason), "CRC 0x%08" PRIX32 " of the tables does not match the "
                           "stored 0x%08" PRIX32 " (tables altered after generation)",
                           rep.crc_computed, rep.crc_stored);
        } else if (st == EMS_ACT_TARGET) {
            (void)snprintf(reason, sizeof(reason), "%s configuration (target %s) in a CONFIG_TARGET=%s build",
                           ems_classification_name(cfg->classification),
                           cfg->target == EMS_TARGET_SIM ? "sim" : "prod", MYEMS_CONFIG_TARGET_NAME);
        } else if (st == EMS_ACT_MAP) {
            (void)snprintf(reason, sizeof(reason), "map %u entry %u: %s", (unsigned)rep.index,
                           (unsigned)rep.sub_index, ems_map_check_name(rep.map_check));
        } else {
            (void)snprintf(reason, sizeof(reason), "%s at index %u/%u", ems_activation_status_name(st),
                           (unsigned)rep.index, (unsigned)rep.sub_index);
        }
        myems_config_rejected(rep.rule, reason, rep.crc_computed);
        return false;
    }
    const ems_sched_plan_status_t ps = ems_sched_plan_build(cfg, &s_plan);
    if (ps != EMS_SCHED_PLAN_OK || s_plan.entry_count == 0U) {
        myems_config_rejected("PLAN", ems_sched_plan_status_name(ps), (uint32_t)ps);
        return false;
    }
    (void)ems_event_emit(EMS_EVT_CONFIG_ACTIVATED, EMS_EVENT_NO_SLOT, EMS_EVENT_NO_PARAM, cfg->crc32,
                         esp_timer_get_time());
    ESP_LOGI(TAG, "EVT_CONFIG_ACTIVATED site=%s crc=0x%08" PRIX32 " classification=%s: %u devices, "
                  "%u poll entries", cfg->site_id, cfg->crc32, ems_classification_name(cfg->classification),
             (unsigned)cfg->device_count, (unsigned)s_plan.entry_count);
    if (cfg->classification != EMS_CLASS_PRODUCTION) {
        ESP_LOGW(TAG, "SIMULATION/TEST CONFIGURATION: synthetic register addresses; "
                      "connect only the MyEMS simulator, never a real meter");
    }
    return true;
}

static uart_parity_t myems_uart_parity(uint8_t parity)
{
    switch (parity) {
    case EMS_PARITY_EVEN: return UART_PARITY_EVEN;
    case EMS_PARITY_ODD:  return UART_PARITY_ODD;
    default:              return UART_PARITY_DISABLE;
    }
}
static void myems_print_diagnostics(ems_poll_scheduler_handle_t scheduler, ems_acquisition_handle_t acquisition,
                                    modbus_master_handle_t master)
{
    ems_poll_scheduler_stats_t ss;
    ems_acquisition_stats_t as;
    if (ems_poll_scheduler_get_stats(scheduler, &ss) != ESP_OK ||
        ems_acquisition_get_stats(acquisition, &as) != ESP_OK) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    uint32_t submitted = 0U, skipped = 0U, rejected = 0U;
    for (uint8_t e = 0; e < ss.entry_count; e++) {
        submitted += ss.entry[e].submitted;
        skipped += ss.entry[e].skipped;
        rejected += ss.entry[e].rejected;
    }
    ESP_LOGI(TAG, "STATS sched: submitted=%" PRIu32 " skipped=%" PRIu32 " rejected=%" PRIu32
                  " guard_expired=%" PRIu32 " fb_unmatched=%" PRIu32 " fb_late=%" PRIu32
                  " health=0x%02" PRIX32 " in_flight=%u bus_holds=%" PRIu32 " bus_cancelled=%" PRIu32
                  " | acq: results=%" PRIu32 " ok=%" PRIu32
                  " failed=%" PRIu32 " cancelled=%" PRIu32 " routing_errors=%" PRIu32 " stale=%" PRIu32,
             submitted, skipped, rejected, ss.core.guard_expired, ss.core.feedback_unmatched,
             ss.core.feedback_late, ss.health_flags, (unsigned)ss.in_flight, ss.core.bus_holds,
             ss.bus_cancelled, as.results, as.ok, as.failed, as.cancelled,
             as.routing_errors, as.stale_transitions);

    for (uint8_t d = 0; d < ems_config_build.device_count; d++) {
        ems_acq_device_stats_t ds;
        if (ems_acquisition_get_device_stats(acquisition, d, &ds) != ESP_OK) {
            continue;
        }
        const long long last_ok_age_ms = ds.last_ok_us > 0 ? (now_us - ds.last_ok_us) / 1000 : -1;
        ESP_LOGI(TAG, "DIAG slot=%u %s mode=%s ok=%" PRIu32 " timeouts=%" PRIu32 " crc=%" PRIu32
                      " protocol=%" PRIu32 " transport=%" PRIu32 " exceptions=%" PRIu32 " last_exc=0x%02X"
                      " mismatches=%" PRIu32 " consec_fail=%u offline=%" PRIu32 " online=%" PRIu32
                      " last_ok_age_ms=%lld counter_disc=%" PRIu32,
                 (unsigned)d, ems_config_build.devices[d].instance_id, ems_acquisition_mode_name(ds.mode),
                 ds.ok, ds.timeouts, ds.crc_errors, ds.protocol_errors, ds.transport_errors, ds.exceptions,
                 (unsigned)ds.last_exception, ds.mismatches, (unsigned)ds.consecutive_failures,
                 ds.offline_transitions, ds.online_transitions, last_ok_age_ms, ds.counter_discontinuities);
    }
    for (uint8_t e = 0; e < ss.entry_count; e++) {
        const ems_sched_entry_t *en = &s_plan.entries[e];
        ESP_LOGI(TAG, "DIAG entry=%u slot=%u grp=%u submitted=%" PRIu32 " skipped=%" PRIu32 " rejected=%" PRIu32
                      " latency_ms=%" PRIu32 " max_latency_ms=%" PRIu32,
                 (unsigned)e, (unsigned)en->slot, (unsigned)en->group_index, ss.entry[e].submitted,
                 ss.entry[e].skipped, ss.entry[e].rejected, ss.entry[e].last_latency_ms,
                 ss.entry[e].max_latency_ms);
    }
    modbus_master_stats_t ms;
    ems_event_stats_t es;
    ems_event_get_stats(&es);
    if (modbus_master_get_stats(master, &ms) == ESP_OK) {
        ESP_LOGI(TAG, "DIAG bus accepted=%" PRIu32 " completed=%" PRIu32 " timeouts=%" PRIu32 " crc=%" PRIu32
                      " protocol=%" PRIu32 " transport=%" PRIu32 " completion_drops=%" PRIu32
                      " backpressure=%" PRIu32 " unexpected_slave=%" PRIu32 " | sched_queue_hwm=%" PRIu32
                      " sched_queue_full=%" PRIu32 " | events=%" PRIu32 " event_drops=%" PRIu32
                      " | health=%s reason=%s transport=%s bus_faults=%" PRIu32 " bus_recoveries=%" PRIu32,
                 ms.requests_accepted, ms.requests_completed, ms.timeouts, ms.crc_errors, ms.protocol_errors,
                 ms.transport_errors, ms.completion_drops, ms.completion_backpressure_rejections,
                 ms.unexpected_slave_frames, ss.queue_hwm, ss.queue_full, es.emitted, es.drops,
                 ems_bus_state_name((ems_bus_state_t)as.bus_state),
                 ems_bus_reason_name((ems_bus_reason_t)as.bus_reason),
                 ems_bus_transport_name((ems_bus_transport_t)as.bus_transport), as.bus_faults,
                 as.bus_recoveries);
    }
}

#endif /* !CONFIG_MYEMS_MODBUS_SELF_TEST */

void app_main(void)
{
#if CONFIG_MYEMS_MODBUS_SELF_TEST
    ESP_LOGW(TAG, "BUILD_MODE=SELF_TEST; connect an RS485 simulator, not a live meter");
#else
    static const ems_event_lock_t event_lock_cfg = { .lock = event_lock, .unlock = event_unlock, .ctx = NULL };
    ems_event_init(&event_lock_cfg);
    ESP_LOGI(TAG, "BUILD_MODE=PRODUCTION CONFIG_TARGET=%s", MYEMS_CONFIG_TARGET_NAME);
#if CONFIG_MYEMS_WP4_ESC_TEST
    ESP_LOGW(TAG, "BUILD_VARIANT=WP4_ESC_TEST: RS-485 recovery is forced to fail (D8 HIL suite esc only)");
#endif
    if (!myems_config_accept(&ems_config_build)) {
        ESP_LOGE(TAG, "No polling started (device stays READY). Fix the configuration or build mode.");
        return;
    }
#endif

    esp_err_t err;
    rs485_handle_t transport = NULL;
    modbus_master_handle_t master = NULL;
#if !CONFIG_MYEMS_MODBUS_SELF_TEST
    ems_poll_scheduler_handle_t scheduler = NULL;
    ems_acquisition_handle_t acquisition = NULL;
#endif

    /* 1. Initialize the physical RS-485 transport.
     * The default GPIOs follow Waveshare's published mapping: TX=17, RX=18,
     * direction/RTS=21. Verify the actual board revision and RTS polarity before
     * connecting a live meter. The board has one onboard physical RS-485 bus.
     */
    rs485_config_t transport_config;
    rs485_config_default(&transport_config);
    transport_config.baud_rate = 9600U;
    transport_config.data_bits = UART_DATA_8_BITS;
    transport_config.parity = UART_PARITY_EVEN;
    transport_config.stop_bits = UART_STOP_BITS_1;
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 3 || CONFIG_MYEMS_WP24_EXTENDED_TEST == 4
    transport_config.recovery_timeout_ms = 500U;
#endif
#if !CONFIG_MYEMS_MODBUS_SELF_TEST
    /* Bus settings from the generated bus table (one onboard bus: bus 0). */
    {
        const ems_cfg_bus_t *bus = &ems_config_build.buses[0];
        transport_config.uart_num = (uart_port_t)bus->uart_num;
        transport_config.tx_gpio = bus->tx_gpio;
        transport_config.rx_gpio = bus->rx_gpio;
        transport_config.rts_gpio = bus->rts_gpio;
        transport_config.baud_rate = bus->baud;
        transport_config.parity = myems_uart_parity(bus->parity);
        transport_config.stop_bits = (bus->stop_bits == 2U) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
        ESP_LOGI(TAG, "bus %s: UART%u TX%u RX%u RTS%u %" PRIu32 " baud parity=%u stop=%u",
                 bus->bus_id, (unsigned)bus->uart_num, (unsigned)bus->tx_gpio, (unsigned)bus->rx_gpio,
                 (unsigned)bus->rts_gpio, bus->baud, (unsigned)bus->parity, (unsigned)bus->stop_bits);
    }
#endif

    err = rs485_open(&transport_config, &transport);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rs485_open failed: %s", esp_err_to_name(err));
        return;
    }

#if CONFIG_MYEMS_WP4_ESC_TEST
#if !CONFIG_MYEMS_WP24_FAULT_INJECTION
#error "CONFIG_MYEMS_WP4_ESC_TEST needs CONFIG_MYEMS_WP24_FAULT_INJECTION (selected in Kconfig)"
#endif
    /* D8 escalation test build (Test Spec ESC-001 "test hook"): from now on an
     * RS-485 recovery never sees bus silence, so it fails after
     * recovery_timeout_ms. The master's fail-closed path, bus health, scheduler
     * hold and device state that follow are the normal code. */
    err = rs485_test_force_recovery_activity(transport, true);
    ESP_LOGW(TAG, "ESC test hook armed: rs485_test_force_recovery_activity -> %s", esp_err_to_name(err));
#endif

    /* 2. Start the asynchronous Modbus master. It owns request serialization,
     * RTU frame assembly, CRC validation, transaction timeouts/retries and the
     * bounded completion queue. It does not know about meter register meanings.
     */
    modbus_master_config_t master_config;
    modbus_master_config_default(&master_config, transport);
    master_config.default_timeout_ms = 500U;
    master_config.default_retries = 1U;

    err = modbus_master_start(&master_config, &master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "modbus_master_start failed: %s", esp_err_to_name(err));
        (void)rs485_close(transport);
        return;
    }

#if CONFIG_MYEMS_MODBUS_SELF_TEST
    /* Self-test mode is deliberately exclusive: no scheduler/acquisition task
     * is started, so the self-test is the only completion-queue consumer.
     * Run it on a dedicated task to keep large test objects off main's stack. */
    myems_self_test_task_args_t self_test_args = {
        .master = master,
        .transport = transport,
        .waiter = xTaskGetCurrentTaskHandle()
    };
    TaskHandle_t self_test_task_handle = NULL;

    if (xTaskCreate(myems_self_test_task,
                    "myems_self_test",
                    MYEMS_SELF_TEST_TASK_STACK_BYTES,
                    &self_test_args,
                    MYEMS_SELF_TEST_TASK_PRIORITY,
                    &self_test_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create self-test task");
        (void)modbus_master_stop(master);
        (void)rs485_close(transport);
        return;
    }

    /* Wait until the self-test has completed and published its result. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "MODE=SELF_TEST result=%s",
             s_self_test_result == ESP_OK ? "PASS" : "FAIL");

    /* Resource ownership remains with app_main. */
    (void)modbus_master_stop(master);
    (void)rs485_close(transport);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000U));
    }
#else
    /* 3. Start the scheduler. It owns poll timing: one entry per (device, poll
     * group), at most one request in flight per device, never waits for a
     * response itself (Design 9.2). */
    const ems_poll_scheduler_config_t scheduler_config = {
        .master = master,
        .plan = &s_plan,
        .task_priority = 5U,
        .task_stack_size = 4096U,
        .feedback_queue_length = 16U,
        .submit_retry_delay_ms = 250U
    };

    err = ems_poll_scheduler_start(&scheduler_config, &scheduler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ems_poll_scheduler_start failed: %s", esp_err_to_name(err));
        (void)modbus_master_stop(master);
        (void)rs485_close(transport);
        return;
    }

    /* 4. Start the acquisition task: sole consumer of the completion queue and
     * sole writer of the state store; routes each result by request ID,
     * decodes, publishes values with quality and feeds every terminal request
     * ID back to the scheduler. */
    const ems_acquisition_config_t acquisition_config = {
        .master = master,
        .transport = transport,             /* bus health (Design 9.7, D8) */
        .scheduler = scheduler,
        .plan = &s_plan,
        .config = &ems_config_build,
        .task_priority = 6U,
        .task_stack_size = 6144U,
        .completion_wait_ms = 100U,
        .value_dump_interval_ms = MYEMS_VALUE_DUMP_MS
    };

    err = ems_acquisition_start(&acquisition_config, &acquisition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ems_acquisition_start failed: %s", esp_err_to_name(err));
        (void)ems_poll_scheduler_stop(scheduler);
        (void)modbus_master_stop(master);
        (void)rs485_close(transport);
        return;
    }

    ESP_LOGI(TAG, "MyEMS DP-WP4 polling started: %u devices, %u poll entries",
             (unsigned)s_plan.device_count, (unsigned)s_plan.entry_count);

    /* app_main does not poll. Every 60 s it prints the diagnostics
     * (Design 9.9): STATS summary, one DIAG line per device and per poll
     * entry, and the bus / queue / event counters. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000U));
        myems_print_diagnostics(scheduler, acquisition, master);
    }
#endif
}
