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
include(embed)

# The picture the jpeg module shows by default is built into the executable.
embed_file("${CMAKE_CURRENT_SOURCE_DIR}/data/picture.jpg"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/default_picture.cpp" bk kDefaultPicture)

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
        src/core/mapped_file.cpp
        src/app/registry.cpp
        src/app/cli.cpp
        src/app/endpoints.cpp
        src/periph/flash.cpp
        src/i2c/catalog.cpp
        src/i2c/register_device.cpp
        src/periph/gdma.cpp
        src/periph/gpio.cpp
        src/periph/i2c1.cpp
        src/periph/intc.cpp
        src/periph/jpeg.cpp
        src/periph/saradc.cpp
        src/periph/sctrl.cpp
        src/periph/timer.cpp
        src/periph/trng.cpp
        src/periph/uart.cpp
        src/periph/sdhost.cpp
        src/periph/unknown.cpp
        src/periph/wifi/frame_engine.cpp
        src/periph/wifi/mac.cpp
        src/net/packets.cpp
        src/net/stack.cpp
        src/net/tcp_flow.cpp
        src/net/station.cpp
        src/net/forwarder.cpp
        src/net/network.cpp
        src/net/pcap.cpp
        src/net/router/crypto.cpp
        src/net/router/access_point.cpp
        src/net/router/authenticator.cpp
        src/net/router/router.cpp
        src/net/router/router_network.cpp
        src/net/router/nat.cpp
        src/net/router/resolver.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/default_picture.cpp"
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
        tests/unit/test_sctrl.cpp
        tests/unit/test_gpio.cpp
        tests/unit/test_gdma.cpp
        tests/unit/test_jpeg.cpp
        tests/unit/test_wifi_mac.cpp
        tests/unit/test_wifi_frame_engine.cpp
        tests/unit/test_station.cpp
        tests/unit/test_packets.cpp
        tests/unit/test_stack.cpp
        tests/unit/test_tcp_flow.cpp
        tests/unit/test_forwarder.cpp
        tests/unit/test_network.cpp
        tests/unit/test_crypto.cpp
        tests/unit/test_access_point.cpp
        tests/unit/test_authenticator.cpp
        tests/unit/test_router.cpp
        tests/unit/test_router_network.cpp
        tests/unit/test_nat.cpp
        tests/unit/test_pcap.cpp
        tests/unit/test_i2c1.cpp
        tests/unit/test_i2c_devices.cpp
        tests/unit/test_trng.cpp
        tests/unit/test_saradc.cpp
        tests/unit/test_sdhost.cpp
        tests/unit/test_unknown.cpp
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
        BKEMU_EXE="$<TARGET_FILE:bkemu>")
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
        ENVIRONMENT "BKEMU_EXE=$<TARGET_FILE:bkemu>")
