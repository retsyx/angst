cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET esp32s3 CACHE STRING "Target MCU")

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Source ESP-Matter's export.sh before building (see README.md)")
endif()
set(ESP_MATTER_PATH "$ENV{ESP_MATTER_PATH}")
set(MATTER_SDK_PATH "${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip")
set(EXTRA_COMPONENT_DIRS
    "${MATTER_SDK_PATH}/config/esp32/components"
    "${ESP_MATTER_PATH}/components")

# Keep first-provisioning inputs outside build/ for backup.
# Provisioned devices use their immutable NVS identity across all OTA images.
# The Go tool vendors its QR library.
set(DISPLAY_MATTER_IDENTITY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated/matter"
    CACHE PATH "Directory containing this device's generated Matter identity")
find_program(DISPLAY_GO_EXECUTABLE go REQUIRED)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "GOCACHE=${CMAKE_CURRENT_SOURCE_DIR}/.cache/go-build"
        "GOMODCACHE=${CMAKE_CURRENT_SOURCE_DIR}/.cache/go-mod"
        "GOPROXY=off" "GOSUMDB=off"
        "${DISPLAY_GO_EXECUTABLE}" run -mod=vendor . --out "${DISPLAY_MATTER_IDENTITY_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/commissioning"
    RESULT_VARIABLE DISPLAY_IDENTITY_RESULT
    OUTPUT_VARIABLE DISPLAY_IDENTITY_OUTPUT
    ERROR_VARIABLE DISPLAY_IDENTITY_ERROR)
if(NOT DISPLAY_IDENTITY_RESULT EQUAL 0)
    message(FATAL_ERROR "Matter identity generation failed: ${DISPLAY_IDENTITY_ERROR}")
endif()
include("${DISPLAY_MATTER_IDENTITY_DIR}/matter_identity.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${DISPLAY_MATTER_IDENTITY_DIR}/identity.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/commissioning/main.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/commissioning/nvs.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/commissioning/payload.go"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/commissioning/go.mod")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(native-display)

if(CONFIG_APP_PROJECT_VER_FROM_CONFIG)
    message(FATAL_ERROR "Disable APP_PROJECT_VER_FROM_CONFIG: firmware versions come from the Go commissioning tool")
endif()

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

# Favor rendering speed for LVGL's per-pixel loops; keep Matter optimized for size.
idf_component_get_property(DISPLAY_LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(${DISPLAY_LVGL_LIB} PRIVATE -O3)

if(CONFIG_LV_TICK_CUSTOM)
    # Read the hardware clock directly, independently of timer task scheduling.
    target_compile_definitions(${DISPLAY_LVGL_LIB} PUBLIC
        "LV_TICK_CUSTOM_SYS_TIME_EXPR=((uint32_t)(esp_timer_get_time()/1000ULL))")
endif()
