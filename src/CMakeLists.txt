# Source list for the `src` component.
idf_component_register(
    SRCS
        "main.cpp"
        "actuator_hal.cpp"
        "sensor_hal.cpp"
        "network_hal.cpp"
        # BacklogStore currently exists in source tree but is not yet integrated
        # into runtime flow; add "backlog_store.cpp" here when enabling offline queueing.
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
)
