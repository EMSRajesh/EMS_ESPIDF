# ems_profile - DP-WP4 4A: types of the generated configuration tables and
# their canonical CRC. Activation checks are added in milestone D7.
# Pure C, no ESP-IDF dependencies; the same sources are built by test/host.
idf_component_register(
    SRCS "src/ems_config_crc.c"
    INCLUDE_DIRS "include"
    REQUIRES ems_register_map
)
