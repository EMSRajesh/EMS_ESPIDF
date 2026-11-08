# Generated configuration tables. Simulation builds choose the site folder with
# CONFIG_MYEMS_SIM_SITE_DIR (D9, DP-WP5 Design 9); other builds use main/generated.
set(MYEMS_GEN_DIR "generated")
if(CONFIG_MYEMS_SIM_SITE_DIR)
    set(MYEMS_GEN_DIR "${CONFIG_MYEMS_SIM_SITE_DIR}")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${MYEMS_GEN_DIR}/ems_config_gen.c")
    message(FATAL_ERROR "CONFIG_MYEMS_SIM_SITE_DIR='${MYEMS_GEN_DIR}': no ems_config_gen.c in main/${MYEMS_GEN_DIR}")
endif()

idf_component_register(
    SRCS
        "app_main.c"
        "ems_sched_core.c"
        "ems_acq_core.c"
        "ems_poll_scheduler.c"
        "ems_acquisition.c"
        "ems_bus_health.c"
        "modbus_self_test.c"
        "${MYEMS_GEN_DIR}/ems_config_gen.c"
    INCLUDE_DIRS "." "include" "${MYEMS_GEN_DIR}"
    REQUIRES
        rs485_transport
        modbus_rtu_master
        esp_timer
        freertos
        ems_stack_diag
        ems_profile
        ems_measurement
        ems_register_map
        ems_diag
)
