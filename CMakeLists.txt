cmake_minimum_required(VERSION 3.20)
project(bk7252_emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(deps)

enable_testing()

# Later tasks add sources via target_sources(bk_core ...).
add_library(bk_core STATIC
        src/core/version.cpp
        src/core/log.cpp
        src/cpu/cpu.cpp
        src/bus/bus.cpp
        src/core/scheduler.cpp
        src/core/machine.cpp
        src/core/profile_loader.cpp
        src/core/loader.cpp
        src/app/registry.cpp
        src/app/cli.cpp
        src/app/endpoints.cpp
        src/periph/flash.cpp
        src/periph/intc.cpp
        src/periph/timer.cpp
        src/periph/uart.cpp
        src/periph/wdt.cpp)
target_include_directories(bk_core PUBLIC src)
target_compile_options(bk_core PRIVATE -Wall -Wextra)
target_link_libraries(bk_core PUBLIC unicorn tomlplusplus::tomlplusplus util)

add_executable(bkemu src/app/main.cpp)
target_compile_options(bkemu PRIVATE -Wall -Wextra)
target_link_libraries(bkemu PRIVATE bk_core)

add_executable(unit_tests
        tests/unit/main.cpp
        tests/unit/test_log.cpp
        tests/unit/test_intc.cpp
        tests/unit/test_timer.cpp
        tests/unit/test_uart.cpp
        tests/unit/test_wdt.cpp
        tests/unit/test_flash.cpp
        tests/unit/test_cpu.cpp
        tests/unit/test_bus.cpp
        tests/unit/test_machine.cpp
        tests/unit/test_loader.cpp
        tests/unit/test_cli.cpp
        tests/unit/test_endpoints.cpp
        tests/unit/test_executable.cpp
        tests/unit/test_unicorn_smoke.cpp)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
target_compile_definitions(unit_tests PRIVATE
        BK_PROFILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/profiles"
        BK7252_EXE="$<TARGET_FILE:bkemu>")
add_dependencies(unit_tests bkemu)
target_link_libraries(unit_tests PRIVATE bk_core doctest::doctest)
add_test(NAME unit COMMAND unit_tests)

# Firmware acceptance checks; need the host tool in the environment variable
# BKLOADER and the images in firmware/, otherwise reported as skipped.
add_test(NAME firmware COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/firmware/run_all.sh)
set_tests_properties(firmware PROPERTIES
        LABELS firmware
        SKIP_RETURN_CODE 77
        TIMEOUT 900
        ENVIRONMENT "BK7252_EXE=$<TARGET_FILE:bkemu>")
