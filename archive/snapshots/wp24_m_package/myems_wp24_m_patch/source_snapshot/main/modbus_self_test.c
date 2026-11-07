#include "modbus_self_test.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "modbus_self_test";
static uint32_t s_request_id = 0x70000000U;

static bool bit_is_set(const modbus_result_t *r, uint16_t bit)
{
    return bit < r->bit_count &&
           (r->bits[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

static esp_err_t run_request(modbus_master_handle_t master,
                             const char *name,
                             modbus_request_t *request,
                             modbus_result_t *result)
{
    request->request_id = ++s_request_id;
    request->slave_addr = 1U; /* Test simulator slave 1 */
    request->timeout_ms = 700U;
    request->max_retries = 1U;

    esp_err_t err = modbus_master_submit(master, request);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: FAIL submit: %s", name, esp_err_to_name(err));
        return err;
    }

    /* One request is in flight at a time; no other consumer may read this queue
       while self-test mode is enabled. */
    err = modbus_master_get_completion(master, result,
                                       pdMS_TO_TICKS(2200U));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: FAIL completion wait: %s", name, esp_err_to_name(err));
        return err;
    }
    if (result->request_id != request->request_id ||
        result->status != MODBUS_RESULT_OK ||
        result->function_code != request->function_code ||
        result->slave_addr != request->slave_addr) {
        ESP_LOGE(TAG, "%s: FAIL status=%d response_fc=%u request_id=%lu expected_id=%lu",
                 name, (int)result->status, result->function_code,
                 (unsigned long)result->request_id,
                 (unsigned long)request->request_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool request_rejected(modbus_master_handle_t master, uint8_t function_code,
                             uint16_t write_count, uint16_t read_count)
{
    modbus_request_t request;
    memset(&request, 0, sizeof(request));
    request.request_id = ++s_request_id;
    request.slave_addr = 1U;
    request.function_code = function_code;
    request.register_count = write_count;
    request.read_register_count = read_count;
    request.timeout_ms = 700U;
    request.max_retries = 0U;
    return modbus_master_submit(master, &request) == ESP_ERR_INVALID_ARG;
}

#define CHECK(cond, name) do { \
    if (!(cond)) { ESP_LOGE(TAG, "%s: FAIL assertion at line %d", name, __LINE__); \
        failures++; } \
    else { ESP_LOGI(TAG, "%s: PASS", name); } \
} while (0)

#if CONFIG_MYEMS_WP24_FAULT_INJECTION && CONFIG_MYEMS_WP24_EXTENDED_TEST > 0 && CONFIG_MYEMS_WP24_EXTENDED_TEST <= 7
static esp_err_t run_wp24_extended_test(modbus_master_handle_t master,
                                        rs485_handle_t transport)
{
    modbus_request_t request = {0};
    modbus_result_t result = {0};
    esp_err_t err;
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 5
    rs485_stats_t wp24_j_before = {0}, wp24_j_after = {0};
    if (rs485_get_stats(transport, &wp24_j_before) != ESP_OK) return ESP_FAIL;
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 6 || CONFIG_MYEMS_WP24_EXTENDED_TEST == 7
    rs485_stats_t wp24_k_before = {0}, wp24_k_after = {0};
    modbus_request_t next_request = {0};
    modbus_result_t next_result = {0};
    uint32_t first_request_id = 0U;
    if (rs485_get_stats(transport, &wp24_k_before) != ESP_OK) return ESP_FAIL;
#endif
    request.request_id = ++s_request_id;
    request.slave_addr = 1U;
    request.function_code = 3U;
    request.start_register = 100U;
    request.register_count = 2U;
    request.timeout_ms = 350U;
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 1
    request.max_retries = 1U;
    ESP_LOGI(TAG, "WP24-G retry test: first matching response must be dropped; retry must follow recovery");
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 2
    request.max_retries = 0U;
    ESP_LOGI(TAG, "WP24-H RX-loss test: injecting a software RS485_EVT_RX_LOSS while request is active");
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 3
    request.max_retries = 0U;
    ESP_LOGI(TAG, "WP24-I recovery-timeout test: forcing recovery activity; bus must remain unavailable");
    ESP_ERROR_CHECK(rs485_test_force_recovery_activity(transport, true));
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 5 || CONFIG_MYEMS_WP24_EXTENDED_TEST == 6 || CONFIG_MYEMS_WP24_EXTENDED_TEST == 7
    request.max_retries = 1U;
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 7
    ESP_LOGI(TAG, "WP24-L stale-response test: simulator delays first FC03/start=100 response 1200 ms and drops retry");
#else
    ESP_LOGI(TAG, "WP24-J/K retry exhaustion: simulator must drop both FC03/start=100 attempts");
#endif
#else
#error "Unsupported MYEMS_WP24_EXTENDED_TEST selection"
#endif

    err = modbus_master_submit(master, &request);
    if (err != ESP_OK) return err;
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 2
    /* Allow TX and enter WAIT_RESPONSE before injecting the synthetic event. */
    vTaskDelay(pdMS_TO_TICKS(100U));
    err = rs485_test_inject_rx_loss(transport, RS485_ERR_RING_BUFFER_FULL);
    if (err != ESP_OK) return err;
#endif

    err = modbus_master_get_completion(master, &result, pdMS_TO_TICKS(5000U));
#if CONFIG_MYEMS_WP24_EXTENDED_TEST == 1
    if (err == ESP_OK && result.request_id == request.request_id &&
        result.status == MODBUS_RESULT_OK && result.retry_count == 1U &&
        result.register_count == 2U && result.registers[0] == 1100U &&
        result.registers[1] == 1101U) {
        ESP_LOGI(TAG, "WP24-G PASS: retry_count=1; successful retry returned [1100,1101]");
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=RETRY");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-G FAIL: wait=%s status=%d retries=%u values=[%u,%u]",
             esp_err_to_name(err), (int)result.status, result.retry_count,
             result.registers[0], result.registers[1]);
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 2
    if (err == ESP_OK && result.request_id == request.request_id &&
        result.status == MODBUS_RESULT_TRANSPORT_ERROR) {
        ESP_LOGI(TAG, "WP24-H PASS: injected RX-loss classified as transport error; recovery completed");
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=RX_LOSS_SOFTWARE_INJECTION");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-H FAIL: wait=%s status=%d", esp_err_to_name(err), (int)result.status);
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 3
    rs485_stats_t transport_stats = {0};
    rs485_recovery_state_t recovery_state = RS485_RECOVERY_IDLE;
    const uint8_t probe_frame[4] = {1U, 3U, 0U, 0U};
    esp_err_t blocked_tx_result;
    (void)rs485_test_force_recovery_activity(transport, false);
    blocked_tx_result = rs485_submit_tx(transport, probe_frame, sizeof(probe_frame), 0x7FFFFFFFU);
    if (err == ESP_OK && result.request_id == request.request_id &&
        result.status == MODBUS_RESULT_TRANSPORT_ERROR &&
        rs485_get_stats(transport, &transport_stats) == ESP_OK &&
        rs485_recovery_get_state(transport, &recovery_state) == ESP_OK &&
        transport_stats.recovery_timeout_count > 0U &&
        recovery_state == RS485_RECOVERY_FAILED &&
        blocked_tx_result == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "WP24-I PASS: recovery_timeout_count=%lu state=FAILED; TX rejected while bus unavailable",
                 (unsigned long)transport_stats.recovery_timeout_count);
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=RECOVERY_TIMEOUT_FAIL_CLOSED");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-I FAIL: wait=%s status=%d timeout_count=%lu recovery_state=%d",
             esp_err_to_name(err), (int)result.status,
             (unsigned long)transport_stats.recovery_timeout_count, (int)recovery_state);
    ESP_LOGE(TAG, "WP24-I TX probe result=%s (expected ESP_ERR_INVALID_STATE)",
             esp_err_to_name(blocked_tx_result));
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 5
    /* WP24-J: the simulator drops both attempts at FC03/start=100. */
    if (rs485_get_stats(transport, &wp24_j_after) == ESP_OK &&
        err == ESP_OK && result.request_id == request.request_id &&
        result.status == MODBUS_RESULT_TIMEOUT && result.retry_count == 1U &&
        (wp24_j_after.recovery_success_count - wp24_j_before.recovery_success_count) >= 2U) {
        ESP_LOGI(TAG, "WP24-J PASS: attempts=2 retries=1 final_status=TIMEOUT; recovery completed twice");
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=RETRY_EXHAUSTION");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-J FAIL: wait=%s id=%lu expected=%lu status=%d retries=%u recovery_delta=%lu",
             esp_err_to_name(err), (unsigned long)result.request_id,
             (unsigned long)request.request_id, (int)result.status,
             result.retry_count,
             (unsigned long)(wp24_j_after.recovery_success_count - wp24_j_before.recovery_success_count));
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 6
    /* WP24-K: after J-style exhaustion, submit a distinct request and verify
     * completion identity, fresh retry state, and data isolation. */
    if (err != ESP_OK || result.request_id != request.request_id ||
        result.status != MODBUS_RESULT_TIMEOUT || result.retry_count != 1U) {
        ESP_LOGE(TAG, "WP24-K FAIL: exhausted request wait=%s id=%lu expected=%lu status=%d retries=%u",
                 esp_err_to_name(err), (unsigned long)result.request_id,
                 (unsigned long)request.request_id, (int)result.status, result.retry_count);
        goto wp24_k_fail;
    }
    first_request_id = result.request_id;
    next_request.request_id = ++s_request_id;
    next_request.slave_addr = 1U;
    next_request.function_code = 3U;
    next_request.start_register = 200U;
    next_request.register_count = 2U;
    next_request.timeout_ms = 1000U;
    next_request.max_retries = 0U;
    if (next_request.request_id == first_request_id ||
        modbus_master_submit(master, &next_request) != ESP_OK) {
        ESP_LOGE(TAG, "WP24-K FAIL: independent request submit failed");
        goto wp24_k_fail;
    }
    err = modbus_master_get_completion(master, &next_result, pdMS_TO_TICKS(5000U));
    if (rs485_get_stats(transport, &wp24_k_after) == ESP_OK &&
        err == ESP_OK && next_result.request_id == next_request.request_id &&
        next_result.status == MODBUS_RESULT_OK && next_result.retry_count == 0U &&
        next_result.register_count == 2U &&
        next_result.registers[0] == 1200U && next_result.registers[1] == 1201U &&
        wp24_k_after.recovery_success_count >= wp24_k_before.recovery_success_count) {
        ESP_LOGI(TAG, "WP24-K PASS: exhausted_id=%lu next_id=%lu next_status=OK retries=0 values=[1200,1201]",
                 (unsigned long)first_request_id, (unsigned long)next_result.request_id);
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=RETRY_EXHAUSTION_REQUEST_ISOLATION");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-K FAIL: next wait=%s id=%lu expected=%lu status=%d retries=%u values=[%u,%u]",
             esp_err_to_name(err), (unsigned long)next_result.request_id,
             (unsigned long)next_request.request_id, (int)next_result.status,
             next_result.retry_count, next_result.registers[0], next_result.registers[1]);
wp24_k_fail:
    ESP_LOGE(TAG, "WP24_EXTENDED_SUMMARY: FAIL mode=RETRY_EXHAUSTION_REQUEST_ISOLATION");
#elif CONFIG_MYEMS_WP24_EXTENDED_TEST == 7
    /* WP24-L: after J-style exhaustion, submit a distinct request and verify
     * completion identity, fresh retry state, and data isolation. */
    if (err != ESP_OK || result.request_id != request.request_id ||
        result.status != MODBUS_RESULT_TIMEOUT || result.retry_count != 1U) {
        ESP_LOGE(TAG, "WP24-L FAIL: exhausted request wait=%s id=%lu expected=%lu status=%d retries=%u",
                 esp_err_to_name(err), (unsigned long)result.request_id,
                 (unsigned long)request.request_id, (int)result.status, result.retry_count);
        goto wp24_l_fail;
    }
    first_request_id = result.request_id;
    next_request.request_id = ++s_request_id;
    next_request.slave_addr = 1U;
    next_request.function_code = 3U;
    next_request.start_register = 200U;
    next_request.register_count = 2U;
    next_request.timeout_ms = 1000U;
    next_request.max_retries = 0U;
    if (next_request.request_id == first_request_id ||
        modbus_master_submit(master, &next_request) != ESP_OK) {
        ESP_LOGE(TAG, "WP24-L FAIL: independent request submit failed");
        goto wp24_l_fail;
    }
    err = modbus_master_get_completion(master, &next_result, pdMS_TO_TICKS(5000U));
    if (rs485_get_stats(transport, &wp24_k_after) == ESP_OK &&
        err == ESP_OK && next_result.request_id == next_request.request_id &&
        next_result.status == MODBUS_RESULT_OK && next_result.retry_count == 0U &&
        next_result.register_count == 2U &&
        next_result.registers[0] == 1200U && next_result.registers[1] == 1201U &&
        wp24_k_after.recovery_success_count >= wp24_k_before.recovery_success_count) {
        ESP_LOGI(TAG, "WP24-L PASS: exhausted_id=%lu next_id=%lu next_status=OK retries=0 values=[1200,1201]",
                 (unsigned long)first_request_id, (unsigned long)next_result.request_id);
        ESP_LOGI(TAG, "WP24_EXTENDED_SUMMARY: PASS mode=LATE_RESPONSE_AFTER_RETRY_EXHAUSTION");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "WP24-L FAIL: next wait=%s id=%lu expected=%lu status=%d retries=%u values=[%u,%u]",
             esp_err_to_name(err), (unsigned long)next_result.request_id,
             (unsigned long)next_request.request_id, (int)next_result.status,
             next_result.retry_count, next_result.registers[0], next_result.registers[1]);
wp24_l_fail:
    ESP_LOGE(TAG, "WP24_EXTENDED_SUMMARY: FAIL mode=LATE_RESPONSE_AFTER_RETRY_EXHAUSTION");
#endif
    ESP_LOGE(TAG, "WP24_EXTENDED_SUMMARY: FAIL mode=%d", CONFIG_MYEMS_WP24_EXTENDED_TEST);
    return ESP_FAIL;
}
#endif

#if CONFIG_MYEMS_WP24_FAULT_INJECTION && CONFIG_MYEMS_WP24_EXTENDED_TEST == 4
/* Unified WP24 campaign. The PC simulator applies this exact sequence to FC03
 * requests at address 100: A drop, B delay, C bad CRC, D short frame,
 * E wrong slave, F exception, G drop, G retry none, H drop, I drop.
 * Requests at address 200 are the isolation/control responses and are unmodified.
 */
static esp_err_t wp24_run_pair(modbus_master_handle_t master, rs485_handle_t transport,
                               unsigned index, int expected_status, bool recovery_expected)
{
    modbus_request_t a = {0}, b = {0};
    modbus_result_t ar = {0}, br = {0};
    rs485_stats_t before = {0}, after = {0};
    esp_err_t ea, eb;
    if (rs485_get_stats(transport, &before) != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "WP24_CASE_START %c", (char)('A' + index));
    a.request_id = ++s_request_id; a.slave_addr = 1U; a.function_code = 3U;
    a.start_register = 100U; a.register_count = 2U; a.timeout_ms = 350U; a.max_retries = 0U;
    ea = modbus_master_submit(master, &a);
    if (ea == ESP_OK) ea = modbus_master_get_completion(master, &ar, pdMS_TO_TICKS(4000U));
    if (ea != ESP_OK || ar.request_id != a.request_id || (int)ar.status != expected_status) {
        ESP_LOGE(TAG, "WP24-%c FAIL A wait=%s status=%d expected=%d",
                 (char)('A' + index), esp_err_to_name(ea), (int)ar.status, expected_status);
        return ESP_FAIL;
    }
    b.request_id = ++s_request_id; b.slave_addr = 1U; b.function_code = 3U;
    b.start_register = 200U; b.register_count = 2U; b.timeout_ms = 1800U; b.max_retries = 0U;
    eb = modbus_master_submit(master, &b);
    if (eb == ESP_OK) eb = modbus_master_get_completion(master, &br, pdMS_TO_TICKS(3500U));
    if (eb != ESP_OK || br.request_id != b.request_id || br.status != MODBUS_RESULT_OK ||
        br.register_count != 2U || br.registers[0] != 1200U || br.registers[1] != 1201U) {
        ESP_LOGE(TAG, "WP24-%c FAIL isolation B wait=%s status=%d values=[%u,%u]",
                 (char)('A' + index), esp_err_to_name(eb), (int)br.status,
                 br.registers[0], br.registers[1]);
        return ESP_FAIL;
    }
    if (rs485_get_stats(transport, &after) != ESP_OK) return ESP_FAIL;
    if (recovery_expected && after.recovery_success_count <= before.recovery_success_count) {
        ESP_LOGE(TAG, "WP24-%c FAIL recovery success counter did not advance", (char)('A' + index));
        return ESP_FAIL;
    }
    if (!recovery_expected && after.recovery_success_count != before.recovery_success_count) {
        ESP_LOGE(TAG, "WP24-F FAIL valid exception unexpectedly triggered recovery");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WP24_%c_PASS A_status=%d B=[1200,1201] recovery=%s",
             (char)('A' + index), (int)ar.status, recovery_expected ? "completed" : "not-needed");
    return ESP_OK;
}

static esp_err_t run_wp24_all_campaign(modbus_master_handle_t master, rs485_handle_t transport)
{
    static const int expected[6] = {
        MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_CRC_ERROR,
        MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_EXCEPTION
    };
    for (unsigned i = 0U; i < 6U; ++i) {
        if (wp24_run_pair(master, transport, i, expected[i], i != 5U) != ESP_OK) return ESP_FAIL;
    }

    /* G: one failed attempt, mandatory recovery, then successful retry. */
    {
        modbus_request_t q = {0}; modbus_result_t r = {0};
        ESP_LOGI(TAG, "WP24_CASE_START G");
        q.request_id = ++s_request_id; q.slave_addr = 1U; q.function_code = 3U;
        q.start_register = 100U; q.register_count = 2U; q.timeout_ms = 350U; q.max_retries = 1U;
        esp_err_t e = modbus_master_submit(master, &q);
        if (e == ESP_OK) e = modbus_master_get_completion(master, &r, pdMS_TO_TICKS(6000U));
        if (e != ESP_OK || r.request_id != q.request_id || r.status != MODBUS_RESULT_OK ||
            r.retry_count != 1U || r.register_count != 2U ||
            r.registers[0] != 1100U || r.registers[1] != 1101U) {
            ESP_LOGE(TAG, "WP24-G FAIL wait=%s status=%d retry=%u values=[%u,%u]",
                     esp_err_to_name(e), (int)r.status, r.retry_count,
                     r.registers[0], r.registers[1]); return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WP24_G_PASS retry_count=1 values=[1100,1101]");
    }

    /* H: deliberately drop the reply, then inject a software RX_LOSS event
     * while the master is still waiting. This is not a physical UART overflow test. */
    {
        modbus_request_t q = {0}; modbus_result_t r = {0}; rs485_stats_t before = {0}, after = {0};
        if (rs485_get_stats(transport, &before) != ESP_OK) return ESP_FAIL;
        ESP_LOGI(TAG, "WP24_CASE_START H");
        q.request_id = ++s_request_id; q.slave_addr = 1U; q.function_code = 3U;
        q.start_register = 100U; q.register_count = 2U; q.timeout_ms = 1200U; q.max_retries = 0U;
        esp_err_t e = modbus_master_submit(master, &q);
        if (e == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(100U)); e = rs485_test_inject_rx_loss(transport, RS485_ERR_RING_BUFFER_FULL); }
        if (e == ESP_OK) e = modbus_master_get_completion(master, &r, pdMS_TO_TICKS(5000U));
        if (e != ESP_OK || r.request_id != q.request_id || r.status != MODBUS_RESULT_TRANSPORT_ERROR) {
            ESP_LOGE(TAG, "WP24-H FAIL wait=%s status=%d", esp_err_to_name(e), (int)r.status); return ESP_FAIL;
        }
        if (rs485_get_stats(transport, &after) != ESP_OK || after.recovery_success_count <= before.recovery_success_count) return ESP_FAIL;
        ESP_LOGI(TAG, "WP24_H_PASS software_RX_LOSS classified=TRANSPORT_ERROR recovery=completed");
    }

    /* I: force recovery activity until the configured 500 ms recovery deadline.
     * It is deliberately last because the bus must remain FAILED and TX blocked. */
    {
        modbus_request_t q = {0}; modbus_result_t r = {0}; rs485_stats_t st = {0};
        rs485_recovery_state_t state = RS485_RECOVERY_IDLE;
        const uint8_t probe[4] = {1U, 3U, 0U, 0U};
        ESP_LOGI(TAG, "WP24_CASE_START I");
        (void)rs485_test_force_recovery_activity(transport, true);
        q.request_id = ++s_request_id; q.slave_addr = 1U; q.function_code = 3U;
        q.start_register = 100U; q.register_count = 2U; q.timeout_ms = 350U; q.max_retries = 0U;
        esp_err_t e = modbus_master_submit(master, &q);
        if (e == ESP_OK) e = modbus_master_get_completion(master, &r, pdMS_TO_TICKS(5000U));
        (void)rs485_test_force_recovery_activity(transport, false);
        esp_err_t tx = rs485_submit_tx(transport, probe, sizeof(probe), 0x7FFFFFFFU);
        if (rs485_get_stats(transport, &st) != ESP_OK ||
            rs485_recovery_get_state(transport, &state) != ESP_OK ||
            e != ESP_OK || r.request_id != q.request_id || r.status != MODBUS_RESULT_TRANSPORT_ERROR ||
            st.recovery_timeout_count == 0U || state != RS485_RECOVERY_FAILED ||
            tx != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "WP24-I FAIL wait=%s status=%d timeouts=%lu state=%d tx=%s",
                     esp_err_to_name(e), (int)r.status, (unsigned long)st.recovery_timeout_count,
                     (int)state, esp_err_to_name(tx)); return ESP_FAIL;
        }
        ESP_LOGI(TAG, "WP24_I_PASS timeout_count=%lu state=FAILED TX=BLOCKED",
                 (unsigned long)st.recovery_timeout_count);
    }
    ESP_LOGI(TAG, "WP24_A_TO_I_SUMMARY: PASS");
    return ESP_OK;
}
#endif

esp_err_t myems_modbus_self_test_run(modbus_master_handle_t master, rs485_handle_t transport)
{
    unsigned failures = 0U;
    modbus_request_t q;
    modbus_result_t r;
    esp_err_t err;

    ESP_LOGI(TAG, "MODE=SELF_TEST; target slave=1; ensure simulator is running on COM12");
    ESP_LOGI(TAG, "Self-test performs writes to test-slave registers/coils 1..8 only.");


#if CONFIG_MYEMS_WP25_B2_TEST
    /*
     * WP2.5-B2 initial lifecycle test.
     * Disconnect the RS485 simulator: the active request must remain pending
     * until stop cancels it. The other two accepted requests must remain queued.
     */
    modbus_request_t stop_req = {0};
    modbus_result_t stop_result = {0};
    uint32_t expected_ids[3] = {0U, 0U, 0U};
    bool ids_seen[3] = {false, false, false};
    unsigned cancelled = 0U;
    bool pass = true;

    stop_req.slave_addr = 1U;
    stop_req.function_code = 3U;
    stop_req.start_register = 0U;
    stop_req.register_count = 1U;
    stop_req.timeout_ms = 5000U;
    stop_req.max_retries = 0U;

    for (unsigned i = 0U; i < 3U; i++) {
        stop_req.request_id = ++s_request_id;
        if (i == 1U) {
            /* Give request 0 time to become the active transaction. */
            vTaskDelay(pdMS_TO_TICKS(100U));
        }
        err = modbus_master_submit(master, &stop_req);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WP25-B2 submit[%u] failed: %s",
                     i, esp_err_to_name(err));
            return ESP_FAIL;
        }
        expected_ids[i] = stop_req.request_id;
    }

    err = modbus_master_stop(master);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WP25-B2 stop failed: %s", esp_err_to_name(err));
        return err;
    }

    modbus_master_lifecycle_state_t lifecycle = MODBUS_MASTER_FAILED;
    err = modbus_master_get_lifecycle_state(master, &lifecycle);
    if (err != ESP_OK || lifecycle != MODBUS_MASTER_DRAINING) {
        ESP_LOGE(TAG, "WP25-B2 expected DRAINING, err=%s state=%d",
                 esp_err_to_name(err), (int)lifecycle);
        pass = false;
    }

    /* Completion queue must remain readable after stop(). */
    for (unsigned i = 0U; i < 3U; i++) {
        err = modbus_master_get_completion(master, &stop_result,
                                           pdMS_TO_TICKS(1000U));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "WP25-B2 completion[%u] missing: %s",
                     i, esp_err_to_name(err));
            pass = false;
            break;
        }

        bool matched = false;
        for (unsigned j = 0U; j < 3U; j++) {
            if (stop_result.request_id == expected_ids[j] && !ids_seen[j]) {
                ids_seen[j] = true;
                matched = true;
                break;
            }
        }
        if (!matched || stop_result.status != MODBUS_RESULT_CANCELLED) {
            ESP_LOGE(TAG, "WP25-B2 unexpected completion id=%lu status=%d",
                     (unsigned long)stop_result.request_id,
                     (int)stop_result.status);
            pass = false;
        } else {
            cancelled++;
        }
    }

    /* A second stop in DRAINING must be harmless and idempotent. */
    esp_err_t second_stop = modbus_master_stop(master);
    if (second_stop != ESP_OK) {
        ESP_LOGE(TAG, "WP25-B2 second stop failed: %s",
                 esp_err_to_name(second_stop));
        pass = false;
    }

    /* Admission must remain closed once stop has entered DRAINING. */
    stop_req.request_id = ++s_request_id;
    esp_err_t submit_after_stop = modbus_master_submit(master, &stop_req);
    if (submit_after_stop != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "WP25-B2 submit after stop returned %s",
                 esp_err_to_name(submit_after_stop));
        pass = false;
    }

    pass = pass && ids_seen[0] && ids_seen[1] && ids_seen[2] &&
           cancelled == 3U && second_stop == ESP_OK &&
           submit_after_stop == ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG,
             "WP25_B2_SUMMARY: %s accepted=3 cancelled=%u "
             "draining=%u second_stop=%s post_stop_submit=%s",
             pass ? "PASS" : "FAIL", cancelled,
             lifecycle == MODBUS_MASTER_DRAINING ? 1U : 0U,
             esp_err_to_name(second_stop),
             esp_err_to_name(submit_after_stop));
    return pass ? ESP_OK : ESP_FAIL;
#endif

#if CONFIG_MYEMS_WP25_D_TEST
    /*
     * WP2.5-D isolated test: no simulator should be connected. Submit one
     * request and allow the master to start it, then queue two more requests.
     * The cancellation command is serviced by the master task, which owns
     * both the active transaction and request queue.
     */
    modbus_request_t cancel_req = {0};
    modbus_result_t cancel_result = {0};
    uint32_t cancelled_count = 0U;
    uint32_t seen_ids[3] = {0U};
    unsigned cancelled_results = 0U;

    cancel_req.slave_addr = 1U;
    cancel_req.function_code = 3U;
    cancel_req.start_register = 0U;
    cancel_req.register_count = 1U;
    cancel_req.timeout_ms = 5000U;
    cancel_req.max_retries = 0U;

    for (unsigned i = 0U; i < 3U; i++) {
        cancel_req.request_id = ++s_request_id;
        if (i == 1U) {
            vTaskDelay(pdMS_TO_TICKS(50U)); /* Let request 0 become active. */
        }
        esp_err_t submit_err = modbus_master_submit(master, &cancel_req);
        if (submit_err != ESP_OK) {
            ESP_LOGE(TAG, "WP25-D submit[%u] failed: %s", i, esp_err_to_name(submit_err));
            return ESP_FAIL;
        }
        seen_ids[i] = cancel_req.request_id;
    }

    esp_err_t cancel_err = modbus_master_cancel_pending(
        master, pdMS_TO_TICKS(1000U), &cancelled_count);
    if (cancel_err != ESP_OK) {
        ESP_LOGE(TAG, "WP25-D cancel API failed: %s", esp_err_to_name(cancel_err));
        return cancel_err;
    }

    bool ids_seen[3] = {false, false, false};
    for (unsigned i = 0U; i < 3U; i++) {
        if (modbus_master_get_completion(master, &cancel_result,
                                         pdMS_TO_TICKS(1000U)) != ESP_OK) {
            ESP_LOGE(TAG, "WP25-D completion[%u] missing", i);
            return ESP_FAIL;
        }
        bool matched = false;
        for (unsigned j = 0U; j < 3U; j++) {
            if (cancel_result.request_id == seen_ids[j] && !ids_seen[j]) {
                ids_seen[j] = true;
                matched = true;
                break;
            }
        }
        if (!matched || cancel_result.status != MODBUS_RESULT_CANCELLED) {
            ESP_LOGE(TAG, "WP25-D unexpected completion id=%lu status=%d",
                     (unsigned long)cancel_result.request_id, (int)cancel_result.status);
            return ESP_FAIL;
        }
        cancelled_results++;
    }

    modbus_master_stats_t cancel_stats = {0};
    if (modbus_master_get_stats(master, &cancel_stats) != ESP_OK) {
        return ESP_FAIL;
    }
    bool all_ids = ids_seen[0] && ids_seen[1] && ids_seen[2];
    bool pass = cancelled_count == 3U && cancelled_results == 3U && all_ids &&
                cancel_stats.requests_accepted == 3U &&
                cancel_stats.requests_completed == 3U &&
                cancel_stats.requests_cancelled == 3U &&
                cancel_stats.active_requests_cancelled == 1U &&
                cancel_stats.queued_requests_cancelled == 2U &&
                cancel_stats.completion_drops == 0U;

    ESP_LOGI(TAG,
             "WP25_D_SUMMARY: %s accepted=%lu cancelled=%lu active=%lu queued=%lu drops=%lu",
             pass ? "PASS" : "FAIL",
             (unsigned long)cancel_stats.requests_accepted,
             (unsigned long)cancel_stats.requests_cancelled,
             (unsigned long)cancel_stats.active_requests_cancelled,
             (unsigned long)cancel_stats.queued_requests_cancelled,
             (unsigned long)cancel_stats.completion_drops);
    return pass ? ESP_OK : ESP_FAIL;
#endif


    /* FC01: simulator slave 1 coils 0..7 start as 0x55 (LSB-first). */
    memset(&q, 0, sizeof(q));
    q.function_code = 1U;
    q.start_register = 0U;
    q.register_count = 8U;

    err = run_request(master, "FC01 read coils", &q, &r);

    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                "FC01 DEBUG: status=%d fc=%u slave=%u "
                "bit_count=%u bits[0]=0x%02X bits[1]=0x%02X",
                (int)r.status,
                r.function_code,
                r.slave_addr,
                r.bit_count,
                r.bits[0],
                r.bits[1]);
    } else {
        ESP_LOGE(TAG,
                "FC01 DEBUG: transaction failed, err=%s",
                esp_err_to_name(err));
    }

    CHECK(err == ESP_OK &&
        r.bit_count == 8U &&
        r.bits[0] == 0x55U,
        "FC01 read coils");

    /* FC02: simulator slave 1 discrete inputs 0..7 start as 0xAA. */
    memset(&q, 0, sizeof(q)); q.function_code=2U; q.start_register=0U; q.register_count=8U;
    err=run_request(master,"FC02 read discrete inputs",&q,&r);
    CHECK(err==ESP_OK && r.bit_count==8U && r.bits[0]==0xAAU,"FC02 read discrete inputs");

    /* FC03: holding registers are slave_id*1000 + address. */
    memset(&q, 0, sizeof(q)); q.function_code=3U; q.start_register=0U; q.register_count=2U;
    err=run_request(master,"FC03 read holding registers",&q,&r);
    CHECK(err==ESP_OK && r.register_count==2U && r.registers[0]==1000U && r.registers[1]==1001U,
          "FC03 read holding registers");

    /* FC04: input registers are slave_id*1000 + 100 + address. */
    memset(&q, 0, sizeof(q)); q.function_code=4U; q.start_register=0U; q.register_count=2U;
    err=run_request(master,"FC04 read input registers",&q,&r);
    CHECK(err==ESP_OK && r.register_count==2U && r.registers[0]==1100U && r.registers[1]==1101U,
          "FC04 read input registers");

    /* FC05: write coil 1 ON, then verify via FC01. */
    memset(&q, 0, sizeof(q)); q.function_code=5U; q.start_register=1U; q.register_count=1U;
    q.write_coils[0]=1U;
    err=run_request(master,"FC05 write single coil",&q,&r);
    CHECK(err==ESP_OK,"FC05 write single coil");
    memset(&q, 0, sizeof(q)); q.function_code=1U; q.start_register=1U; q.register_count=1U;
    err=run_request(master,"FC05 readback",&q,&r);
    CHECK(err==ESP_OK && r.bit_count==1U && bit_is_set(&r,0U),"FC05 readback");

    /* FC06: write holding register 5 and verify with FC03. */
    memset(&q, 0, sizeof(q)); q.function_code=6U; q.start_register=5U; q.register_count=1U;
    q.write_registers[0]=0x1234U;
    err=run_request(master,"FC06 write single register",&q,&r);
    CHECK(err==ESP_OK,"FC06 write single register");
    memset(&q, 0, sizeof(q)); q.function_code=3U; q.start_register=5U; q.register_count=1U;
    err=run_request(master,"FC06 readback",&q,&r);
    CHECK(err==ESP_OK && r.register_count==1U && r.registers[0]==0x1234U,"FC06 readback");

    /* FC15: write packed pattern 0x55 to coils 1..8, then verify. */
    memset(&q, 0, sizeof(q)); q.function_code=15U; q.start_register=1U; q.register_count=8U;
    q.write_coils[0]=0x55U;
    err=run_request(master,"FC15 write multiple coils",&q,&r);
    CHECK(err==ESP_OK,"FC15 write multiple coils");
    memset(&q, 0, sizeof(q)); q.function_code=1U; q.start_register=1U; q.register_count=8U;
    err=run_request(master,"FC15 readback",&q,&r);
    CHECK(err==ESP_OK && r.bit_count==8U && r.bits[0]==0x55U,"FC15 readback");

    /* FC16: write two holding registers and verify with FC03. */
    memset(&q, 0, sizeof(q)); q.function_code=16U; q.start_register=5U; q.register_count=2U;
    q.write_registers[0]=0x1234U; q.write_registers[1]=0xABCDU;
    err=run_request(master,"FC16 write multiple registers",&q,&r);
    CHECK(err==ESP_OK,"FC16 write multiple registers");
    memset(&q, 0, sizeof(q)); q.function_code=3U; q.start_register=5U; q.register_count=2U;
    err=run_request(master,"FC16 readback",&q,&r);
    CHECK(err==ESP_OK && r.register_count==2U && r.registers[0]==0x1234U && r.registers[1]==0xABCDU,
          "FC16 readback");

    /* FC23 writes holding registers 5..6, while reading holding registers 0..1. */
    memset(&q, 0, sizeof(q)); q.function_code=23U;
    q.start_register=5U; q.register_count=2U; q.write_registers[0]=0x2222U; q.write_registers[1]=0x3333U;
    q.read_start_register=0U; q.read_register_count=2U;
    err=run_request(master,"FC23 read/write multiple registers",&q,&r);
    CHECK(err==ESP_OK && r.register_count==2U && r.registers[0]==1000U && r.registers[1]==1001U,
          "FC23 read/write multiple registers");
    memset(&q, 0, sizeof(q)); q.function_code=3U; q.start_register=5U; q.register_count=2U;
    err=run_request(master,"FC23 write readback",&q,&r);
    CHECK(err==ESP_OK && r.register_count==2U && r.registers[0]==0x2222U && r.registers[1]==0x3333U,
          "FC23 write readback");

    /* Local validation tests: these requests must be rejected before TX.
     * FC23 has a stricter write limit than FC16 because its request carries
     * both read and write ranges in one 256-byte RTU ADU. */
    CHECK(request_rejected(master, 23U, MODBUS_MAX_FC23_WRITE_REGISTERS + 1U, 1U),
          "VALIDATE FC23 write quantity 122 rejected");
    CHECK(request_rejected(master, 23U, 1U, MODBUS_MAX_REGISTERS + 1U),
          "VALIDATE FC23 read quantity 126 rejected");
    CHECK(request_rejected(master, 16U, MODBUS_MAX_WRITE_REGISTERS + 1U, 0U),
          "VALIDATE FC16 write quantity 124 rejected");
    CHECK(request_rejected(master, 3U, MODBUS_MAX_REGISTERS + 1U, 0U),
          "VALIDATE FC03 read quantity 126 rejected");

#if CONFIG_MYEMS_WP24_FAULT_INJECTION && CONFIG_MYEMS_WP24_EXTENDED_TEST == 4
#if CONFIG_MYEMS_WP24_CASE_SELECTION > 0
    /* Isolated WP24-A..F mode. The selected case still performs its address-200
     * isolation/control transaction, but does not advance to unrelated cases. */
    {
        static const int selected_expected_status[6] = {
            MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_CRC_ERROR,
            MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_TIMEOUT, MODBUS_RESULT_EXCEPTION
        };
        const unsigned selected_index = (unsigned)CONFIG_MYEMS_WP24_CASE_SELECTION - 1U;
        const char selected_case = (char)('A' + selected_index);
        if (failures != 0U ||
            wp24_run_pair(master, transport, selected_index,
                          selected_expected_status[selected_index],
                          selected_index != 5U) != ESP_OK) {
            ESP_LOGE(TAG, "SELF_TEST_SUMMARY: FAIL WP24 selected case=%c", selected_case);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "SELF_TEST_SUMMARY: PASS WP24 selected case=%c", selected_case);
        return ESP_OK;
    }
#else
    if (failures != 0U || run_wp24_all_campaign(master, transport) != ESP_OK) {
        ESP_LOGE(TAG, "SELF_TEST_SUMMARY: FAIL WP24 A-I campaign");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "SELF_TEST_SUMMARY: PASS WP24 A-I campaign");
    return ESP_OK;
#endif
#elif CONFIG_MYEMS_WP24_FAULT_INJECTION && CONFIG_MYEMS_WP24_EXTENDED_TEST > 0
    err = run_wp24_extended_test(master, transport);
    if (err == ESP_OK && failures == 0U) {
        ESP_LOGI(TAG, "SELF_TEST_SUMMARY: PASS baseline FC/validation and selected WP2.4 extended test");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "SELF_TEST_SUMMARY: FAIL WP2.4 extended test");
    return ESP_FAIL;
#endif

    /*
     * WP2 stale-response isolation test (simulator fault mode: stale-fc03).
     * A: slave 1 FC03 address 100 qty 2; simulator delays its response 1000 ms.
     * A times out at 350 ms. B: same slave/function/quantity, address 200.
     * Simulator delays B's genuine response 1200 ms, so stale A arrives first
     * while B is waiting. Standard FC03 response does not echo start address.
     */
    {
        modbus_request_t a_req = {0};
        modbus_request_t b_req = {0};
        modbus_result_t a_result = {0};
        modbus_result_t b_result = {0};
        esp_err_t a_submit, a_wait, b_submit, b_wait;

        a_req.request_id = ++s_request_id;
        a_req.slave_addr = 1U;
        a_req.function_code = 3U;
        a_req.start_register = 100U;
        a_req.register_count = 2U;
        a_req.timeout_ms = 350U;
        a_req.max_retries = 0U;

        a_submit = modbus_master_submit(master, &a_req);
        /* A failure is not published until the master-side late-response guard
         * and transport recovery/release have completed. 900 ms was shorter
         * than the configured 1000 ms guard, so it could leave A's completion
         * in the queue and let B consume the wrong completion. */
        a_wait = (a_submit == ESP_OK)
            ? modbus_master_get_completion(master, &a_result, pdMS_TO_TICKS(3500U))
            : a_submit;

        if (a_wait != ESP_OK) {
            ESP_LOGE(TAG, "WP2-A completion wait failed: submit=%s wait=%s",
                     esp_err_to_name(a_submit), esp_err_to_name(a_wait));
        } else {
            ESP_LOGI(TAG, "WP2-A completion: id=%lu expected_id=%lu status=%d",
                     (unsigned long)a_result.request_id,
                     (unsigned long)a_req.request_id, (int)a_result.status);
        }
        /*
         * WP24 fault-injection cases intentionally produce different failure
         * classes for request A:
         *   dropped/delayed/short/wrong-slave response -> TIMEOUT
         *   corrupted frame                         -> CRC_ERROR
         *   valid Modbus exception                  -> EXCEPTION
         *
         * Keep this firmware-level isolation assertion fault-agnostic. The
         * host runner validates the exact expected status for each selected
         * simulator fault. A must still complete with its own request ID and
         * a non-OK protocol result; the subsequent B check proves that A did
         * not consume or contaminate B's completion.
         */
        const bool a_failure_status_expected =
            (a_result.status == MODBUS_RESULT_TIMEOUT) ||
            (a_result.status == MODBUS_RESULT_CRC_ERROR) ||
            (a_result.status == MODBUS_RESULT_PROTOCOL_ERROR) ||
            (a_result.status == MODBUS_RESULT_EXCEPTION) ||
            (a_result.status == MODBUS_RESULT_TRANSPORT_ERROR);

        CHECK(a_submit == ESP_OK && a_wait == ESP_OK &&
              a_result.request_id == a_req.request_id &&
              a_failure_status_expected,
              "WP2-A fault response classified and isolated");

        b_req.request_id = ++s_request_id;
        b_req.slave_addr = 1U;
        b_req.function_code = 3U;
        b_req.start_register = 200U;
        b_req.register_count = 2U;
        b_req.timeout_ms = 1600U;
        b_req.max_retries = 0U;

        b_submit = modbus_master_submit(master, &b_req);
        /* B starts only after A's completion has been consumed. Allow a
         * bounded interval for queueing, TX, slave response and completion. */
        b_wait = (b_submit == ESP_OK)
            ? modbus_master_get_completion(master, &b_result, pdMS_TO_TICKS(3500U))
            : b_submit;

        if (b_wait != ESP_OK) {
            ESP_LOGE(TAG, "WP2-B completion wait failed: submit=%s wait=%s",
                     esp_err_to_name(b_submit), esp_err_to_name(b_wait));
        } else {
            ESP_LOGI(TAG, "WP2-B completion: id=%lu expected_id=%lu status=%d count=%u",
                     (unsigned long)b_result.request_id,
                     (unsigned long)b_req.request_id,
                     (int)b_result.status, b_result.register_count);
        }
        if (b_wait == ESP_OK && b_result.request_id == b_req.request_id &&
            b_result.status == MODBUS_RESULT_OK) {
            ESP_LOGI(TAG,
                     "WP2-B request B result: status=%d registers=[%u,%u] expected=[1200,1201]",
                     (int)b_result.status, b_result.registers[0], b_result.registers[1]);
        }

        CHECK(b_submit == ESP_OK && b_wait == ESP_OK &&
              b_result.request_id == b_req.request_id &&
              b_result.status == MODBUS_RESULT_OK &&
              b_result.register_count == 2U &&
              b_result.registers[0] == 1200U &&
              b_result.registers[1] == 1201U,
              "WP2-B stale response A must not satisfy request B");
    }

    if (failures == 0U) {
        ESP_LOGI(TAG, "SELF_TEST_SUMMARY: PASS function-code, validation and WP2 stale-response tests");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "SELF_TEST_SUMMARY: FAIL assertions=%u", failures);
    return ESP_FAIL;
}
