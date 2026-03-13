idf_component_register(
    SRCS
        "main.cpp"
        "led_controller.cpp"
    INCLUDE_DIRS
        "."
        "../include"
    REQUIRES led_strip esp_driver_gpio
)
