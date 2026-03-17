#pragma once

// Shared data models used across application modules.
// Keep this file free of heavy dependencies so it can be included widely.

#include <cstdint>
#include <cmath>

// One telemetry snapshot captured by the device at a specific time.
struct TelemetrySample {
  // Monotonic sequence number for ordering telemetry packets.
  uint32_t seq = 0;
  // Milliseconds since boot when this sample was taken.
  uint64_t uptime_ms = 0;
  // Unix epoch time in milliseconds at capture; 0 means unavailable/not synchronized.
  uint64_t captured_unix_ms = 0;
  // Environmental measurements; NAN means "not available".
  float temperature_c = NAN;
  float humidity_pct = NAN;
  float pressure_hpa = NAN;
  // Battery voltage measured at sampling time.
  float battery_v = NAN;
  // Whether sensor reads for this sample are considered valid.
  bool sensor_ok = false;
};
