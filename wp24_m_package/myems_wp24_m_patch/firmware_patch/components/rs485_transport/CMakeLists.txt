idf_component_register(
    SRCS "src/rs485_transport.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_timer freertos ems_stack_diag
)
