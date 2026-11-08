# ems_profile - DP-WP4 4A: types of the generated configuration tables, their
# canonical CRC, and the firmware activation checks (D7).
# Pure C, no ESP-IDF dependencies; the same sources are built by test/host.
idf_component_register(
    SRCS
        "src/ems_config_crc.c"
        "src/ems_profile_activate.c"
    INCLUDE_DIRS "include"
    REQUIRES ems_register_map ems_measurement
)
