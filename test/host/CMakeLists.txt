# MyEMS host unit tests (DP-WP4).
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

# --------------------------------------------------------------- tests ---
add_executable(test_ems_measurement
    test_main.c
    test_ems_dictionary.c
    test_ems_state_store.c)
target_link_libraries(test_ems_measurement PRIVATE ems_measurement unity)
target_compile_options(test_ems_measurement PRIVATE ${EMS_WARNINGS})

if(EMS_HOST_SANITIZE AND NOT MSVC)
    foreach(t ems_measurement test_ems_measurement)
        target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
endif()
if(EMS_HOST_COVERAGE AND NOT MSVC)
    foreach(t ems_measurement test_ems_measurement)
        target_compile_options(${t} PRIVATE --coverage -O0)
        target_link_options(${t} PRIVATE --coverage)
    endforeach()
endif()

enable_testing()
add_test(NAME ems_measurement COMMAND test_ems_measurement)
