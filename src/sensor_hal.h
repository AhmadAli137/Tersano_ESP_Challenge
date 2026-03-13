#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_adc/adc_oneshot.h"

#include "app_types.h"

class SensorHal {
 public:
  SensorHal(uint8_t sda_pin, uint8_t scl_pin, uint8_t battery_adc_pin);
  void begin();
  TelemetrySample read(uint32_t seq);

 private:
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
  bool initBme280();
  bool readBme280(float& temperature_c, float& humidity_pct, float& pressure_hpa);
  float readBatteryVoltage() const;

  uint8_t sda_pin_;
  uint8_t scl_pin_;
  uint8_t battery_adc_pin_;
  uint8_t bme_addr_ = 0x76;
  bool bme_ok_ = false;
  BmeCalib calib_ = {};
  int32_t t_fine_ = 0;
  adc_oneshot_unit_handle_t adc_handle_ = nullptr;

  float fake_temp_ = 23.0f;
  float fake_hum_ = 55.0f;
  float fake_pressure_ = 1009.0f;
};
