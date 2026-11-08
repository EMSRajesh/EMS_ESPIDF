# ems_diag - DP-WP4 diagnostics support: event ring (Design 9.9).
# Pure C, no ESP-IDF dependencies; the same sources are built by test/host.
idf_component_register(
    SRCS
        "src/ems_event.c"
    INCLUDE_DIRS "include"
)
