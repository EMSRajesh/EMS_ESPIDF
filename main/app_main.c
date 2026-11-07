#include <inttypes.h>

#include "ems_acquisition.h"
#include "ems_config_gen.h"
#include "ems_poll_scheduler.h"
#include "ems_sched_core.h"
#include "ems_stack_diag.h"
#include "esp_err.h"
#include "esp_log.h"
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
 * meter profiles are gone. Full activation (structural re-validation, events)
 * is milestone D7; D5 checks the CRC and the build target and builds the
 * scheduler plan. On any failure nothing is polled.
 */
#if CONFIG_MYEMS_CONFIG_TARGET_SIM
#define MYEMS_CONFIG_TARGET_NAME "sim"
#define MYEMS_CONFIG_TARGET_VALUE EMS_TARGET_SIM
#else
#define MYEMS_CONFIG_TARGET_NAME "prod"
#define MYEMS_CONFIG_TARGET_VALUE EMS_TARGET_PROD
#endif

static ems_sched_plan_t s_plan;

static bool myems_config_accept(const ems_config_build_t *cfg)
{
    ESP_LOGI(TAG, "CONFIG site=%s target=%s classification=%s crc=0x%08" PRIX32 " devices=%u",
             cfg->site_id, cfg->target == EMS_TARGET_SIM ? "sim" : "prod",
             ems_classification_name(cfg->classification), cfg->crc32, (unsigned)cfg->device_count);
    ESP_LOGI(TAG, "CONFIG inputs: %s (sha256 %s, tool %s, umd %s)",
             cfg->inputs_summary, cfg->inputs_sha256, cfg->tool_version, cfg->umd_version);

    const uint32_t crc = ems_config_crc32(cfg);
    if (crc != cfg->crc32) {
        ESP_LOGE(TAG, "CONFIG REJECTED: CRC 0x%08" PRIX32 " does not match stored 0x%08" PRIX32,
                 crc, cfg->crc32);
        return false;
    }
    /* V-13 (firmware side): a simulation build accepts only sim tables and a
     * production build only production tables. */
    if (cfg->target != MYEMS_CONFIG_TARGET_VALUE ||
        (MYEMS_CONFIG_TARGET_VALUE == EMS_TARGET_PROD && cfg->classification != EMS_CLASS_PRODUCTION)) {
        ESP_LOGE(TAG, "CONFIG REJECTED: %s configuration (target %s) in a CONFIG_TARGET=%s build",
                 ems_classification_name(cfg->classification),
                 cfg->target == EMS_TARGET_SIM ? "sim" : "prod", MYEMS_CONFIG_TARGET_NAME);
        return false;
    }
    if (cfg->bus_count < 1U) {
        ESP_LOGE(TAG, "CONFIG REJECTED: no bus");
        return false;
    }
    const ems_sched_plan_status_t ps = ems_sched_plan_build(cfg, &s_plan);
    if (ps != EMS_SCHED_PLAN_OK || s_plan.entry_count == 0U) {
        ESP_LOGE(TAG, "CONFIG REJECTED: scheduler plan %s (%u entries)",
                 ems_sched_plan_status_name(ps), (unsigned)s_plan.entry_count);
        return false;
    }
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
#endif /* !CONFIG_MYEMS_MODBUS_SELF_TEST */

void app_main(void)
{
#if CONFIG_MYEMS_MODBUS_SELF_TEST
    ESP_LOGW(TAG, "BUILD_MODE=SELF_TEST; connect an RS485 simulator, not a live meter");
#else
    ESP_LOGI(TAG, "BUILD_MODE=PRODUCTION CONFIG_TARGET=%s", MYEMS_CONFIG_TARGET_NAME);
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

    /* 4. Start the acquisition task: sole consumer of the completion queue;
     * routes each result by request ID and feeds every terminal request ID
     * back to the scheduler. */
    const ems_acquisition_config_t acquisition_config = {
        .master = master,
        .scheduler = scheduler,
        .plan = &s_plan,
        .config = &ems_config_build,
        .task_priority = 6U,
        .task_stack_size = 4096U,
        .completion_wait_ms = 100U
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

    /* app_main does not poll. Every 60 s it prints the scheduler and
     * acquisition counters (diagnostics proper come in D7). */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000U));
        ems_poll_scheduler_stats_t ss;
        ems_acquisition_stats_t as;
        if (ems_poll_scheduler_get_stats(scheduler, &ss) == ESP_OK &&
            ems_acquisition_get_stats(acquisition, &as) == ESP_OK) {
            uint32_t submitted = 0U, skipped = 0U, rejected = 0U;
            for (uint8_t e = 0; e < ss.entry_count; e++) {
                submitted += ss.entry[e].submitted;
                skipped += ss.entry[e].skipped;
                rejected += ss.entry[e].rejected;
            }
            ESP_LOGI(TAG, "STATS sched: submitted=%" PRIu32 " skipped=%" PRIu32 " rejected=%" PRIu32
                          " guard_expired=%" PRIu32 " fb_unmatched=%" PRIu32 " fb_late=%" PRIu32
                          " health=0x%02" PRIX32 " | acq: results=%" PRIu32 " ok=%" PRIu32
                          " failed=%" PRIu32 " routing_errors=%" PRIu32,
                     submitted, skipped, rejected, ss.core.guard_expired, ss.core.feedback_unmatched,
                     ss.core.feedback_late, ss.health_flags, as.results, as.ok, as.failed,
                     as.routing_errors);
        }
    }
#endif
}
