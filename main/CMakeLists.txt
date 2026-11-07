idf_component_register(
    SRCS
        "app_main.c"
        "ems_poll_scheduler.c"
        "ems_acquisition.c"
        "modbus_self_test.c"
    INCLUDE_DIRS "." "include"
    REQUIRES
        rs485_transport
        modbus_rtu_master
        esp_timer
        freertos
        ems_stack_diag
)
