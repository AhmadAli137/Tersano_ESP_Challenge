#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#include "app_types.h"

/*
 * SensorHal owns environmental + battery acquisition.
 *
 * Current implementation:
 * - BME280 over I2C for temperature/humidity/pressure
 * - ADC one-shot for battery voltage
 * - emits N/A (NaN) values when BME280 is unavailable
 */
class SensorHal {
 public:
  SensorHal(uint8_t sda_pin, uint8_t scl_pin, uint8_t battery_adc_pin);
  // Initialize I2C bus, ADC channel, and probe/configure BME280.
  void begin();
  // Read one telemetry sample, including timestamp and health flag.
  TelemetrySample read(uint32_t seq);

 private:
  // Sensor calibration parameters read from BME280 NVM.
  struct BmeCalib {
    uint16_t dig_t1 = 0;
    int16_t dig_t2 = 0;
    int16_t dig_t3 = 0;
    uint16_t dig_p1 = 0;
    int16_t dig_p2 = 0;
    int16_t dig_p3 = 0;
    int16_t dig_p4 = 0;
    int16_t dig_p5 = 0;
    int16_t dig_p6 = 0;
    int16_t dig_p7 = 0;
    int16_t dig_p8 = 0;
    int16_t dig_p9 = 0;
    uint8_t dig_h1 = 0;
    int16_t dig_h2 = 0;
    uint8_t dig_h3 = 0;
    int16_t dig_h4 = 0;
    int16_t dig_h5 = 0;
    int8_t dig_h6 = 0;
  };

  bool writeReg(uint8_t reg, uint8_t value) const;
  bool readRegs(uint8_t reg, uint8_t* out, size_t len) const;
  // Detect BME280 address, load calibration constants, configure oversampling/mode.
  bool initBme280();
  // Read raw registers and convert to physical units.
  bool readBme280(float& temperature_c, float& humidity_pct, float& pressure_hpa);
  // Convert raw ADC reading into battery voltage.
  float readBatteryVoltage() const;

  // Board pin mapping.
  uint8_t sda_pin_;
  uint8_t scl_pin_;
  uint8_t battery_adc_pin_;
  // BME280 I2C address (0x76 or 0x77).
  uint8_t bme_addr_ = 0x76;
  // True when real sensor data path is available.
  bool bme_ok_ = false;
  BmeCalib calib_ = {};
  // BME280 compensation intermediate shared across T/P/H calculations.
  int32_t t_fine_ = 0;
  adc_oneshot_unit_handle_t adc_handle_ = nullptr;
  adc_cali_handle_t adc_cali_handle_ = nullptr;
  bool adc_cali_enabled_ = false;
};
