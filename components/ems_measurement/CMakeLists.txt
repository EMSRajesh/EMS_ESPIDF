# ems_measurement - DP-WP4 4B: measurement dictionary, quality, state store.
# Pure C, no ESP-IDF dependencies; the same sources are built by test/host.
# src/ems_param_table.c and include/ems_param_ids.h are GENERATED from
# config/umd/umd_v1.json by tools/profile/umd_gen.py.
idf_component_register(
    SRCS
        "src/ems_measurement.c"
        "src/ems_param_table.c"
        "src/ems_state_store.c"
    INCLUDE_DIRS "include"
)
