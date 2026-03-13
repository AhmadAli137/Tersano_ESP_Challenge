#pragma once

/*
 * LED controller module.
 * Owns all LED hardware details so higher-level app code can stay simple.
 *
 * Supported backends (selected by Kconfig):
 * - Addressable LED strip (RMT or SPI backend)
 * - Plain GPIO LED
 */

#include <stdbool.h>

#include "esp_err.h"

namespace led_controller {

// Initialize LED hardware and set a known OFF state.
esp_err_t init();

// Set logical LED state: true = on, false = off.
esp_err_t set_on(bool on);

// Toggle LED and apply the new state.
esp_err_t toggle();

// Read cached logical state.
bool is_on();

}  // namespace led_controller
