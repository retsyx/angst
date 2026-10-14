idf_component_register(
    SRCS "main.cpp" "board_display.c" "service.c" "display_settings.c" "matter_display.cpp"
         "matter_provisioning.c" "matter_bootstrap.c"
         "ota_server.c" "upgrade_password.c" "password_display.cpp" "bodoni_72_smallcaps.c"
         "animation_test.c" "price_test.c" "price_test_math.c"
         "lighting_settings.c" "led_lighting.c"
    INCLUDE_DIRS "." "${DISPLAY_MATTER_IDENTITY_DIR}"
    REQUIRES lvgl__lvgl esp_lcd esp_driver_spi esp_driver_gpio esp_driver_ledc esp_timer
             esp_wifi esp_event esp_netif nvs_flash esp_http_client http_parser mbedtls
             esp_http_server app_update esp_system espressif__mdns esp_matter esp_matter_console
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CHIP_HAVE_CONFIG_H)
