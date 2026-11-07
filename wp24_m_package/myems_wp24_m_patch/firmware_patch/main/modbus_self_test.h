#pragma once
#include "esp_err.h"
#include "modbus_rtu_master.h"

esp_err_t myems_modbus_self_test_run(modbus_master_handle_t master, rs485_handle_t transport);
