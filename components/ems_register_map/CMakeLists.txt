# ems_register_map - DP-WP4 4C: Modbus register map engine.
# Decode (types, byte/word order), scale/offset, CT/PT ratio, range check,
# map validation and energy counter tracking.
# Pure C, no ESP-IDF dependencies; the same sources are built by test/host.
idf_component_register(
    SRCS
        "src/ems_decode.c"
        "src/ems_convert.c"
        "src/ems_counter.c"
    INCLUDE_DIRS "include"
)
