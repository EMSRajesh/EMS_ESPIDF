# Generated configuration tables: lab_bench by default; other simulation
# sites (D9) are selected with the MYEMS_SIM_SITE choice in sdkconfig.
set(MYEMS_GEN_DIR "generated")
if(CONFIG_MYEMS_SIM_SITE_LAB_2DEV)
    set(MYEMS_GEN_DIR "generated_sites/lab_bench_2dev")
elseif(CONFIG_MYEMS_SIM_SITE_LAB_CT200)
    set(MYEMS_GEN_DIR "generated_sites/lab_ratio_ct200")
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
