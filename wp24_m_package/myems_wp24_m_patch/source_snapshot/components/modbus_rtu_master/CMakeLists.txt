idf_component_register(
    SRCS "src/modbus_rtu_master.c"
    INCLUDE_DIRS "include"
    REQUIRES rs485_transport esp_timer freertos ems_stack_diag
)
