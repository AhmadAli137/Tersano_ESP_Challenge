/*
 * LED controller implementation.
 * Encapsulates backend-specific setup and write operations.
 */

#include "include/led_controller.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "led_strip.h"
#include "sdkconfig.h"

namespace {

// Module log tag.
constexpr char kTag[] = "led_controller";

// Fallback defaults for projects that have not defined these in Kconfig yet.
#ifndef CONFIG_BLINK_GPIO
#define CONFIG_BLINK_GPIO 27
#endif

#if !defined(CONFIG_BLINK_LED_STRIP) && !defined(CONFIG_BLINK_LED_GPIO)
#define CONFIG_BLINK_LED_STRIP 1
#endif

#if defined(CONFIG_BLINK_LED_STRIP) && \
    !defined(CONFIG_BLINK_LED_STRIP_BACKEND_RMT) && \
    !defined(CONFIG_BLINK_LED_STRIP_BACKEND_SPI)
#define CONFIG_BLINK_LED_STRIP_BACKEND_RMT 1
#endif

constexpr gpio_num_t kBlinkGpio = static_cast<gpio_num_t>(CONFIG_BLINK_GPIO);

// Keep a local logical state so callers do not need to track it.
bool s_led_on = false;
bool s_initialized = false;

#ifdef CONFIG_BLINK_LED_STRIP
led_strip_handle_t s_led_strip = nullptr;
#endif

// Apply requested state to the currently selected backend.
esp_err_t apply_state(bool on)
{
#ifdef CONFIG_BLINK_LED_STRIP
    if (on) {
        // Dim white to keep current draw low.
        esp_err_t err = led_strip_set_pixel(s_led_strip, 0, 16, 16, 16);
        if (err != ESP_OK) {
            return err;
        }
        return led_strip_refresh(s_led_strip);
    }
    return led_strip_clear(s_led_strip);
#elif CONFIG_BLINK_LED_GPIO
    return gpio_set_level(kBlinkGpio, on ? 1 : 0);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

}  // namespace

namespace led_controller {

esp_err_t init()
{
    if (s_initialized) {
        // Idempotent init makes module safe to call from restarts/retries.
        return ESP_OK;
    }

#ifdef CONFIG_BLINK_LED_STRIP
    ESP_LOGI(kTag, "Using addressable LED strip on GPIO %d", static_cast<int>(kBlinkGpio));

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = kBlinkGpio;
    strip_config.max_leds = 1;

#if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.flags.with_dma = false;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (err != ESP_OK) {
        return err;
    }
#elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
    led_strip_spi_config_t spi_config = {};
    spi_config.spi_bus = SPI2_HOST;
    spi_config.flags.with_dma = true;

    esp_err_t err = led_strip_new_spi_device(&strip_config, &spi_config, &s_led_strip);
    if (err != ESP_OK) {
        return err;
    }
#else
#error "Unsupported LED strip backend"
#endif

#elif CONFIG_BLINK_LED_GPIO
    ESP_LOGI(kTag, "Using GPIO LED on GPIO %d", static_cast<int>(kBlinkGpio));
    ESP_RETURN_ON_ERROR(gpio_reset_pin(kBlinkGpio), kTag, "gpio_reset_pin failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(kBlinkGpio, GPIO_MODE_OUTPUT), kTag, "gpio_set_direction failed");

#else
#error "Unsupported LED type"
#endif

    s_led_on = false;
    ESP_RETURN_ON_ERROR(apply_state(false), kTag, "failed to set initial LED state");
    s_initialized = true;
    return ESP_OK;
}

esp_err_t set_on(bool on)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(apply_state(on), kTag, "failed to update LED state");
    s_led_on = on;
    return ESP_OK;
}

esp_err_t toggle()
{
    return set_on(!s_led_on);
}

bool is_on()
{
    return s_led_on;
}

}  // namespace led_controller
