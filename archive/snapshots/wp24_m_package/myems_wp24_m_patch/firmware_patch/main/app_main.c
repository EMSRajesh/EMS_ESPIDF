#include "ems_acquisition.h"
#include "ems_stack_diag.h"
#include "ems_poll_scheduler.h"
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


/* --------------------------------------------------------------------------
 * DEMONSTRATION METER PROFILES
 * --------------------------------------------------------------------------
 * These are placeholder register settings to demonstrate two meters on one
 * shared RS-485 multidrop bus. Replace the slave IDs, function codes, starting
 * registers, register counts, baud rate and serial format using each meter's
 * actual manual. Register address zero is NOT universally voltage.
 *
 * The slave IDs must be unique on the same physical RS-485 bus. The initial
 * offsets spread the two polls apart so they do not become due simultaneously.
 */
#define GRID_METER_SLAVE_ADDRESS       1U
#define LOAD_METER_SLAVE_ADDRESS       2U
#define DEMO_FUNCTION_CODE             0x03U
#define DEMO_START_REGISTER            0U
#define DEMO_REGISTER_COUNT            2U
#define METER_POLL_PERIOD_MS           5000U

#if !CONFIG_MYEMS_MODBUS_SELF_TEST
static const ems_meter_profile_t s_meter_profiles[] = {
    {
        .name = "grid_meter",
        .slave_addr = GRID_METER_SLAVE_ADDRESS,
        .function_code = DEMO_FUNCTION_CODE,
        .start_register = DEMO_START_REGISTER,
        .register_count = DEMO_REGISTER_COUNT,
        .poll_period_ms = METER_POLL_PERIOD_MS,
        .initial_offset_ms = 500U,
        .timeout_ms = 500U,
        .max_retries = 1U
    },
    {
        .name = "load_meter",
        .slave_addr = LOAD_METER_SLAVE_ADDRESS,
        .function_code = DEMO_FUNCTION_CODE,
        .start_register = DEMO_START_REGISTER,
        .register_count = DEMO_REGISTER_COUNT,
        .poll_period_ms = METER_POLL_PERIOD_MS,
        .initial_offset_ms = 3000U,
        .timeout_ms = 500U,
        .max_retries = 1U
    }
};
#endif /* !CONFIG_MYEMS_MODBUS_SELF_TEST */

void app_main(void)
{
#if CONFIG_MYEMS_MODBUS_SELF_TEST
    ESP_LOGW(TAG, "BUILD_MODE=SELF_TEST; connect an RS485 simulator, not a live meter");
#else
    ESP_LOGI(TAG, "BUILD_MODE=PRODUCTION");
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
    /* 3. Start the scheduler. It submits periodic requests but never waits for
     * a meter response. If a meter already has a request in flight, the next
     * polling occurrence is skipped instead of flooding the request queue.
     */
    const ems_poll_scheduler_config_t scheduler_config = {
        .master = master,
        .meters = s_meter_profiles,
        .meter_count = sizeof(s_meter_profiles) / sizeof(s_meter_profiles[0]),
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

    /* 4. Start a dedicated acquisition task. It is the sole consumer of the
     * Modbus completion queue, processes results, and feeds terminal request IDs
     * back to the scheduler so each meter's in-flight slot can be released.
     */
    const ems_acquisition_config_t acquisition_config = {
        .master = master,
        .scheduler = scheduler,
        .meters = s_meter_profiles,
        .meter_count = sizeof(s_meter_profiles) / sizeof(s_meter_profiles[0]),
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

    ESP_LOGI(TAG, "MyEMS asynchronous RS-485/Modbus application started");
    ESP_LOGI(TAG, "One shared RS-485 bus; grid and load meters are polled independently");
    ESP_LOGI(TAG, "Poll period=%u ms per meter; configure profiles from actual meter manuals",
             METER_POLL_PERIOD_MS);

    /* app_main deliberately does not contain a polling loop. The scheduler,
     * Modbus master, acquisition and transport tasks run independently under
     * FreeRTOS. Keep this task alive with a long sleep and let the other tasks
     * perform the actual work. */
    (void)acquisition;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000U));
    }
#endif
}
