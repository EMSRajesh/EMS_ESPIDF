# MyEMS host unit tests (DP-WP4: D1 ems_measurement, D2 ems_register_map,
# D3 ems_profile + generated configuration tables, D5 poll scheduler core,
# D6 acquisition core, D7 activation checks and event ring).
#
# Builds the pure-C DP-WP4 components with the PC's compiler and runs them
# with the Unity framework that ships inside ESP-IDF:
#     %IDF_PATH%/components/unity/unity
# Override with -DUNITY_DIR=<path to a Unity checkout>.
#
# Build and run (from the project root):
#     cmake -S test/host -B build_host
#     cmake --build build_host
#     ctest --test-dir build_host --output-on-failure
#
# Options:
#     -DEMS_HOST_SANITIZE=ON   AddressSanitizer + UBSan (GCC/Clang, not MinGW)
#     -DEMS_HOST_COVERAGE=ON   gcov instrumentation (GCC/Clang)
cmake_minimum_required(VERSION 3.16)
project(myems_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(EMS_HOST_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
option(EMS_HOST_COVERAGE "Build with coverage instrumentation" OFF)

get_filename_component(MYEMS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# ---------------------------------------------------------------- Unity ---
if(NOT DEFINED UNITY_DIR)
    if(DEFINED ENV{IDF_PATH})
        set(UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity")
    endif()
endif()
if(NOT UNITY_DIR OR NOT EXISTS "${UNITY_DIR}/src/unity.c")
    message(FATAL_ERROR
        "Unity not found. Run from an ESP-IDF PowerShell (IDF_PATH set) or pass "
        "-DUNITY_DIR=<dir containing src/unity.c>. Looked in: '${UNITY_DIR}'")
endif()
message(STATUS "Unity: ${UNITY_DIR}")

add_library(unity STATIC "${UNITY_DIR}/src/unity.c")
target_include_directories(unity PUBLIC "${UNITY_DIR}/src")
target_compile_definitions(unity PUBLIC UNITY_INCLUDE_DOUBLE UNITY_SUPPORT_64)

# ------------------------------------------------------- warning policy ---
if(MSVC)
    set(EMS_WARNINGS /W4 /WX)
else()
    set(EMS_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror)
endif()

# ------------------------------------------- component under test (4B) ---
set(EMS_MEAS_DIR "${MYEMS_ROOT}/components/ems_measurement")
add_library(ems_measurement STATIC
    "${EMS_MEAS_DIR}/src/ems_measurement.c"
    "${EMS_MEAS_DIR}/src/ems_param_table.c"
    "${EMS_MEAS_DIR}/src/ems_state_store.c")
target_include_directories(ems_measurement PUBLIC "${EMS_MEAS_DIR}/include")
target_compile_options(ems_measurement PRIVATE ${EMS_WARNINGS})
if(NOT MSVC)
    target_link_libraries(ems_measurement PUBLIC m)
endif()

# ---------------------------------- component under test (4C, DP-WP4 D2) ---
set(EMS_REGMAP_DIR "${MYEMS_ROOT}/components/ems_register_map")
add_library(ems_register_map STATIC
    "${EMS_REGMAP_DIR}/src/ems_decode.c"
    "${EMS_REGMAP_DIR}/src/ems_convert.c"
    "${EMS_REGMAP_DIR}/src/ems_counter.c")
target_include_directories(ems_register_map PUBLIC "${EMS_REGMAP_DIR}/include")
target_compile_options(ems_register_map PRIVATE ${EMS_WARNINGS})
if(NOT MSVC)
    target_link_libraries(ems_register_map PUBLIC m)
endif()

# ------------------------------------ component under test (4A, DP-WP4 D3) ---
set(EMS_PROFILE_DIR "${MYEMS_ROOT}/components/ems_profile")
add_library(ems_profile STATIC
    "${EMS_PROFILE_DIR}/src/ems_config_crc.c"
    "${EMS_PROFILE_DIR}/src/ems_profile_activate.c")
target_include_directories(ems_profile PUBLIC "${EMS_PROFILE_DIR}/include")
target_link_libraries(ems_profile PUBLIC ems_register_map ems_measurement)

# ------------------------------------------- event ring (DP-WP4 D7) ---
set(EMS_DIAG_DIR "${MYEMS_ROOT}/components/ems_diag")
add_library(ems_diag STATIC "${EMS_DIAG_DIR}/src/ems_event.c")
target_include_directories(ems_diag PUBLIC "${EMS_DIAG_DIR}/include")
target_compile_options(ems_diag PRIVATE ${EMS_WARNINGS})
target_compile_options(ems_profile PRIVATE ${EMS_WARNINGS})

# ------------------------------- scheduler core (4E, DP-WP4 D5), in main/ ---
add_library(ems_sched_core STATIC "${MYEMS_ROOT}/main/ems_sched_core.c")
target_include_directories(ems_sched_core PUBLIC "${MYEMS_ROOT}/main/include")
target_link_libraries(ems_sched_core PUBLIC ems_profile)
target_compile_options(ems_sched_core PRIVATE ${EMS_WARNINGS})

# --------------------------------------------------------------- tests ---
add_executable(test_ems_measurement
    test_main.c
    test_ems_dictionary.c
    test_ems_state_store.c)
target_link_libraries(test_ems_measurement PRIVATE ems_measurement unity)
target_compile_options(test_ems_measurement PRIVATE ${EMS_WARNINGS})

# The register map tests use parameter IDs from ems_measurement (header only).
add_executable(test_ems_register_map
    test_main_regmap.c
    test_ems_register_map.c
    test_ems_counter.c)
target_link_libraries(test_ems_register_map PRIVATE ems_register_map ems_measurement unity)
target_compile_options(test_ems_register_map PRIVATE ${EMS_WARNINGS})

# Generated tables (tools/profile/myems_profile_tool.py generate). Each set
# defines 'ems_config_build', so each gets its own executable:
#   lab_bench    -> main/generated/                      (the firmware's copy)
#   decoder_test -> test/host/generated/decoder_test/    (test fixture)
foreach(cfg lab decoder)
    if(cfg STREQUAL "lab")
        set(gen_dir "${MYEMS_ROOT}/main/generated")
    else()
        set(gen_dir "${CMAKE_CURRENT_LIST_DIR}/generated/decoder_test")
    endif()
    add_executable(test_ems_config_${cfg}
        test_main_config_${cfg}.c
        test_ems_config_common.c
        test_ems_config_${cfg}.c
        "${gen_dir}/ems_config_gen.c")
    target_include_directories(test_ems_config_${cfg} PRIVATE "${gen_dir}")
    target_link_libraries(test_ems_config_${cfg} PRIVATE ems_profile ems_register_map ems_measurement unity)
    target_compile_options(test_ems_config_${cfg} PRIVATE ${EMS_WARNINGS})
endforeach()

add_executable(test_ems_sched_core
    test_main_sched.c
    test_ems_sched_core.c)
target_link_libraries(test_ems_sched_core PRIVATE ems_sched_core unity)
target_compile_options(test_ems_sched_core PRIVATE ${EMS_WARNINGS})

# Acquisition core (D6): runs on the generated lab_bench tables.
add_executable(test_ems_acq_core
    test_main_acq.c
    test_ems_acq_core.c
    "${MYEMS_ROOT}/main/ems_acq_core.c"
    "${MYEMS_ROOT}/main/generated/ems_config_gen.c")
target_include_directories(test_ems_acq_core PRIVATE "${MYEMS_ROOT}/main/generated")
target_link_libraries(test_ems_acq_core PRIVATE ems_sched_core ems_profile ems_register_map ems_measurement unity)
target_compile_options(test_ems_acq_core PRIVATE ${EMS_WARNINGS})

# Activation checks and event ring (D7).
add_executable(test_ems_d7
    test_main_d7.c
    test_ems_profile_activate.c
    test_ems_event.c
    "${MYEMS_ROOT}/main/generated/ems_config_gen.c")
target_include_directories(test_ems_d7 PRIVATE "${MYEMS_ROOT}/main/generated")
target_link_libraries(test_ems_d7 PRIVATE ems_profile ems_diag ems_register_map ems_measurement unity)
target_compile_options(test_ems_d7 PRIVATE ${EMS_WARNINGS})

# Bus health (D8, Design 9.7): pure C, no dependencies.
add_library(ems_bus_health STATIC "${MYEMS_ROOT}/main/ems_bus_health.c")
target_include_directories(ems_bus_health PUBLIC "${MYEMS_ROOT}/main/include")
target_compile_options(ems_bus_health PRIVATE ${EMS_WARNINGS})

add_executable(test_ems_d8
    test_main_d8.c
    test_ems_bus_health.c)
target_link_libraries(test_ems_d8 PRIVATE ems_bus_health unity)
target_compile_options(test_ems_d8 PRIVATE ${EMS_WARNINGS})

# Per-site firmware tables (D9 build modes Wp4Sim2Dev, Wp4Ratio; DP-WP5 Wp5Dc, Wp5Mixed):
# one executable per site.
set(site_dir_2dev   "lab_bench_2dev")
set(site_dir_ct200  "lab_ratio_ct200")
set(site_dir_dc     "lab_dc_bench")
set(site_dir_mixed  "lab_mixed")
foreach(site 2dev ct200 dc mixed)
    set(site_dir "${MYEMS_ROOT}/main/generated_sites/${site_dir_${site}}")
    string(TOUPPER "${site}" SITE_UPPER)
    add_executable(test_ems_config_site_${site}
        test_main_config_site.c
        test_ems_config_common.c
        test_ems_config_site.c
        "${site_dir}/ems_config_gen.c")
    target_include_directories(test_ems_config_site_${site} PRIVATE "${site_dir}")
    target_compile_definitions(test_ems_config_site_${site} PRIVATE EMS_TEST_SITE_${SITE_UPPER})
    target_link_libraries(test_ems_config_site_${site} PRIVATE ems_sched_core ems_profile ems_register_map ems_measurement unity)
    target_compile_options(test_ems_config_site_${site} PRIVATE ${EMS_WARNINGS})
endforeach()

set(EMS_HOST_TARGETS ems_measurement ems_register_map ems_profile ems_sched_core ems_diag ems_bus_health
    test_ems_measurement test_ems_register_map test_ems_config_lab test_ems_config_decoder
    test_ems_sched_core test_ems_acq_core test_ems_d7 test_ems_d8
    test_ems_config_site_2dev test_ems_config_site_ct200 test_ems_config_site_dc test_ems_config_site_mixed)
if(EMS_HOST_SANITIZE AND NOT MSVC)
    foreach(t ${EMS_HOST_TARGETS})
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
if(EMS_HOST_COVERAGE AND NOT MSVC)
    foreach(t ${EMS_HOST_TARGETS})
        target_compile_options(${t} PRIVATE --coverage -O0)
        target_link_options(${t} PRIVATE --coverage)
    endforeach()
endif()

enable_testing()
add_test(NAME ems_measurement COMMAND test_ems_measurement)
add_test(NAME ems_register_map COMMAND test_ems_register_map)
add_test(NAME ems_config_lab COMMAND test_ems_config_lab)
add_test(NAME ems_config_decoder COMMAND test_ems_config_decoder)
add_test(NAME ems_sched_core COMMAND test_ems_sched_core)
add_test(NAME ems_acq_core COMMAND test_ems_acq_core)
add_test(NAME ems_d7 COMMAND test_ems_d7)
add_test(NAME ems_d8 COMMAND test_ems_d8)
add_test(NAME ems_config_site_2dev COMMAND test_ems_config_site_2dev)
add_test(NAME ems_config_site_ct200 COMMAND test_ems_config_site_ct200)
add_test(NAME ems_config_site_dc COMMAND test_ems_config_site_dc)
add_test(NAME ems_config_site_mixed COMMAND test_ems_config_site_mixed)
