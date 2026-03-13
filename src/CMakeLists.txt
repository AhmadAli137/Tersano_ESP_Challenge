# ESP-IDF component registration for project application sources.
#
# Notes:
# - `src` is used as the component root (instead of default `main` folder)
# - include paths expose both local headers and shared `include/`
# - REQUIRES lists direct component dependencies used by these sources
idf_component_register(
    SRCS
        "main.cpp"
        "rtu_controller.cpp"
        "actuator_hal.cpp"
        "sensor_hal.cpp"
        "network_hal.cpp"
        "backlog_store.cpp"
    INCLUDE_DIRS
        "."
        "../include"
    # Component dependencies exported to this component's compile/link context.
    REQUIRES
        led_strip
        esp_driver_gpio
        esp_driver_i2c
        driver
        esp_adc
        esp_timer
        esp_http_client
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        json
        mbedtls
        spiffs
)
