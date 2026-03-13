#include "sensor_hal.h"

#include <algorithm>
#include <cmath>

#include "driver/i2c.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "app_config.h"

namespace {
constexpr const char* kTag = "SensorHal";
constexpr i2c_port_t kI2cPort = I2C_NUM_0;
constexpr uint32_t kI2cFreqHz = 100000;
}  // namespace

SensorHal::SensorHal(uint8_t sda_pin, uint8_t scl_pin, uint8_t battery_adc_pin)
    : sda_pin_(sda_pin), scl_pin_(scl_pin), battery_adc_pin_(battery_adc_pin) {}

void SensorHal::begin() {
  // Configure master I2C bus shared with BME280.
  i2c_config_t cfg = {};
  cfg.mode = I2C_MODE_MASTER;
  cfg.sda_io_num = static_cast<gpio_num_t>(sda_pin_);
  cfg.scl_io_num = static_cast<gpio_num_t>(scl_pin_);
  cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
  cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
  cfg.master.clk_speed = kI2cFreqHz;
  i2c_param_config(kI2cPort, &cfg);
  i2c_driver_install(kI2cPort, cfg.mode, 0, 0, 0);

  // Configure battery ADC channel for one-shot sampling.
  adc_oneshot_unit_init_cfg_t adc_cfg = {};
  adc_cfg.unit_id = ADC_UNIT_1;
  adc_oneshot_new_unit(&adc_cfg, &adc_handle_);
  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_12;
  chan_cfg.bitwidth = ADC_BITWIDTH_12;
  adc_oneshot_config_channel(adc_handle_, appcfg::BATTERY_ADC_CHANNEL, &chan_cfg);

  bme_ok_ = initBme280();
  ESP_LOGI(kTag, "BME280 init: %s", bme_ok_ ? "ok" : "fallback");
}

TelemetrySample SensorHal::read(uint32_t seq) {
  TelemetrySample sample = {};
  sample.seq = seq;
  sample.uptime_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
  sample.battery_v = readBatteryVoltage();

  float t = NAN, h = NAN, p = NAN;
  if (bme_ok_ && readBme280(t, h, p) && !std::isnan(t) && !std::isnan(h) && !std::isnan(p)) {
    sample.temperature_c = t;
    sample.humidity_pct = h;
    sample.pressure_hpa = p;
    sample.sensor_ok = true;
    return sample;
  }

  // Fallback waveform keeps downstream pipeline exercised without hard failure.
  fake_temp_ += 0.05f;
  if (fake_temp_ > 27.0f) fake_temp_ = 23.0f;
  fake_hum_ += 0.08f;
  if (fake_hum_ > 63.0f) fake_hum_ = 55.0f;
  fake_pressure_ += 0.09f;
  if (fake_pressure_ > 1019.0f) fake_pressure_ = 1009.0f;

  sample.temperature_c = fake_temp_;
  sample.humidity_pct = fake_hum_;
  sample.pressure_hpa = fake_pressure_;
  sample.sensor_ok = false;
  return sample;
}

bool SensorHal::writeReg(uint8_t reg, uint8_t value) const {
  uint8_t payload[2] = {reg, value};
  return i2c_master_write_to_device(kI2cPort, bme_addr_, payload, sizeof(payload), pdMS_TO_TICKS(100)) == ESP_OK;
}

bool SensorHal::readRegs(uint8_t reg, uint8_t* out, size_t len) const {
  return i2c_master_write_read_device(kI2cPort, bme_addr_, &reg, 1, out, len, pdMS_TO_TICKS(100)) == ESP_OK;
}

bool SensorHal::initBme280() {
  // Probe common BME280 addresses.
  uint8_t id = 0;
  bme_addr_ = 0x76;
  if (!readRegs(0xD0, &id, 1) || id != 0x60) {
    bme_addr_ = 0x77;
    if (!readRegs(0xD0, &id, 1) || id != 0x60) return false;
  }

  uint8_t calib1[26] = {};
  uint8_t calib2[7] = {};
  if (!readRegs(0x88, calib1, sizeof(calib1))) return false;
  if (!readRegs(0xE1, calib2, sizeof(calib2))) return false;

  calib_.dig_t1 = static_cast<uint16_t>(calib1[1] << 8 | calib1[0]);
  calib_.dig_t2 = static_cast<int16_t>(calib1[3] << 8 | calib1[2]);
  calib_.dig_t3 = static_cast<int16_t>(calib1[5] << 8 | calib1[4]);
  calib_.dig_p1 = static_cast<uint16_t>(calib1[7] << 8 | calib1[6]);
  calib_.dig_p2 = static_cast<int16_t>(calib1[9] << 8 | calib1[8]);
  calib_.dig_p3 = static_cast<int16_t>(calib1[11] << 8 | calib1[10]);
  calib_.dig_p4 = static_cast<int16_t>(calib1[13] << 8 | calib1[12]);
  calib_.dig_p5 = static_cast<int16_t>(calib1[15] << 8 | calib1[14]);
  calib_.dig_p6 = static_cast<int16_t>(calib1[17] << 8 | calib1[16]);
  calib_.dig_p7 = static_cast<int16_t>(calib1[19] << 8 | calib1[18]);
  calib_.dig_p8 = static_cast<int16_t>(calib1[21] << 8 | calib1[20]);
  calib_.dig_p9 = static_cast<int16_t>(calib1[23] << 8 | calib1[22]);
  calib_.dig_h1 = calib1[25];
  calib_.dig_h2 = static_cast<int16_t>(calib2[1] << 8 | calib2[0]);
  calib_.dig_h3 = calib2[2];
  calib_.dig_h4 = static_cast<int16_t>((calib2[3] << 4) | (calib2[4] & 0x0F));
  calib_.dig_h5 = static_cast<int16_t>((calib2[5] << 4) | (calib2[4] >> 4));
  calib_.dig_h6 = static_cast<int8_t>(calib2[6]);

  if (!writeReg(0xF2, 0x01)) return false;
  if (!writeReg(0xF4, 0x27)) return false;
  if (!writeReg(0xF5, 0xA0)) return false;
  return true;
}

bool SensorHal::readBme280(float& temperature_c, float& humidity_pct, float& pressure_hpa) {
  // Read contiguous P/T/H measurement block.
  uint8_t data[8] = {};
  if (!readRegs(0xF7, data, sizeof(data))) return false;

  const int32_t adc_p = (static_cast<int32_t>(data[0]) << 12) | (static_cast<int32_t>(data[1]) << 4) | (data[2] >> 4);
  const int32_t adc_t = (static_cast<int32_t>(data[3]) << 12) | (static_cast<int32_t>(data[4]) << 4) | (data[5] >> 4);
  const int32_t adc_h = (static_cast<int32_t>(data[6]) << 8) | data[7];

  // Compensation math per BME280 datasheet fixed-point formulas.
  int32_t var1 = ((((adc_t >> 3) - (static_cast<int32_t>(calib_.dig_t1) << 1))) * static_cast<int32_t>(calib_.dig_t2)) >> 11;
  int32_t var2 = (((((adc_t >> 4) - static_cast<int32_t>(calib_.dig_t1)) *
                    ((adc_t >> 4) - static_cast<int32_t>(calib_.dig_t1))) >> 12) *
                  static_cast<int32_t>(calib_.dig_t3)) >>
                 14;
  t_fine_ = var1 + var2;
  const int32_t t = (t_fine_ * 5 + 128) >> 8;
  temperature_c = static_cast<float>(t) / 100.0f;

  int64_t pvar1 = static_cast<int64_t>(t_fine_) - 128000;
  int64_t pvar2 = pvar1 * pvar1 * static_cast<int64_t>(calib_.dig_p6);
  pvar2 += (pvar1 * static_cast<int64_t>(calib_.dig_p5)) << 17;
  pvar2 += static_cast<int64_t>(calib_.dig_p4) << 35;
  pvar1 = ((pvar1 * pvar1 * static_cast<int64_t>(calib_.dig_p3)) >> 8) + ((pvar1 * static_cast<int64_t>(calib_.dig_p2)) << 12);
  pvar1 = (((static_cast<int64_t>(1) << 47) + pvar1) * static_cast<int64_t>(calib_.dig_p1)) >> 33;
  if (pvar1 == 0) return false;
  int64_t p = 1048576 - adc_p;
  p = (((p << 31) - pvar2) * 3125) / pvar1;
  pvar1 = (static_cast<int64_t>(calib_.dig_p9) * (p >> 13) * (p >> 13)) >> 25;
  pvar2 = (static_cast<int64_t>(calib_.dig_p8) * p) >> 19;
  p = ((p + pvar1 + pvar2) >> 8) + (static_cast<int64_t>(calib_.dig_p7) << 4);
  pressure_hpa = static_cast<float>(p) / 25600.0f;

  int32_t h = t_fine_ - 76800;
  h = (((((adc_h << 14) - (static_cast<int32_t>(calib_.dig_h4) << 20) - (static_cast<int32_t>(calib_.dig_h5) * h)) + 16384) >> 15) *
       (((((((h * static_cast<int32_t>(calib_.dig_h6)) >> 10) * (((h * static_cast<int32_t>(calib_.dig_h3)) >> 11) + 32768)) >> 10) + 2097152) *
         static_cast<int32_t>(calib_.dig_h2) + 8192) >>
        14));
  h = h - (((((h >> 15) * (h >> 15)) >> 7) * static_cast<int32_t>(calib_.dig_h1)) >> 4);
  h = std::max<int32_t>(0, std::min<int32_t>(h, 419430400));
  humidity_pct = static_cast<float>(h >> 12) / 1024.0f;
  return true;
}

float SensorHal::readBatteryVoltage() const {
  int raw = 0;
  if (adc_oneshot_read(adc_handle_, appcfg::BATTERY_ADC_CHANNEL, &raw) != ESP_OK) return NAN;
  // Convert ADC code -> pin voltage -> pre-divider battery voltage.
  const float adc_v = (static_cast<float>(raw) / static_cast<float>(appcfg::ADC_MAX)) * appcfg::ADC_REF_VOLTAGE;
  return adc_v * appcfg::BATTERY_DIVIDER_RATIO;
}
