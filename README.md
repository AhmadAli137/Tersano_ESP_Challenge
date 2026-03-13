# Tersano ESP Challenge (ESP32-C5 RTU)

This project is ESP-IDF C++ firmware for an ESP32-C5 remote telemetry unit (RTU).
It is designed to keep collecting data even during network/cloud interruptions, then recover automatically.

The firmware does five main jobs:

1. Samples environment + battery measurements.
2. Publishes telemetry and status to Supabase over HTTPS.
3. Polls cloud commands and applies them on-device.
4. Drives local actuators (RGB status LED + piezo buzzer).
5. Buffers unsent telemetry in SPIFFS backlog, then flushes later.

## Who This README Is For

Use this guide if you are:

1. Bringing up this project on a new board.
2. Debugging publish/backlog behavior from serial logs.
3. Changing timing, pins, or table names.
4. Operating the RTU and wanting clear expectations for LED/log behavior.

## System Mental Model

Think of the firmware as three loops running in parallel:

1. `sampleTask`: produce samples on a fixed interval.
2. `publishTask`: send fresh sample immediately; if it fails, cache it.
3. `connectivityTask`: maintain network/command polling and drain old cached rows.

This gives you both low latency (fresh sample send) and reliability (persistent backlog).

## End-to-End Data Flow

### Normal path (online)

1. Sensor sample created.
2. Sample queued.
3. Publish task sends sample to Supabase.
4. Log shows `Telemetry sent`.
5. Nothing is added to backlog.

### Offline/degraded path

1. Sensor sample created.
2. Publish attempt fails (transport/TLS/HTTP failure).
3. Sample appended to `/spiffs/backlog.ndjson`.
4. Log shows `Offline, cached (N lines)`.
5. Connectivity task keeps retrying in background.

### Recovery path (after connectivity returns)

1. Connectivity task pops oldest backlog line.
2. Attempts publish.
3. On success: row removed permanently, log `Flushed backlog (N left)`.
4. On failure: row is re-prepended and retried later.

Important: backlog flush is intentionally one row per connectivity loop to avoid monopolizing network time.

## Telemetry Send vs Backlog Flush (What You See In Logs)

You may see logs like:

1. `Telemetry sent`
2. `Flushed backlog (117 left)`
3. `Telemetry sent`
4. `Flushed backlog (116 left)`

That is expected and healthy:

1. Fresh live samples are still being sent.
2. Older cached samples are drained in parallel.
3. HTTP access is serialized by a mutex in `NetworkHal`, so publish/poll/flush do not collide.

## LED State Behavior

Status LED color is controlled by RTU state:

1. Red: Wi-Fi disconnected.
2. Yellow: Wi-Fi up but cloud publish path is degraded (repeated failures/stale success window).
3. Blue: connected and intentionally running slower-than-default sampling interval.
4. Green: connected and healthy with normal/default cadence.

Why you may still see mostly green:

1. Short transient failures can recover before degradation threshold is crossed.
2. Successful sends reset failure streak.
3. If failures are occasional, system returns to healthy state quickly.

## Intermittent Publish Failures (Why They Happen)

You may still see occasional lines like:

1. `mbedtls_ssl_handshake returned -0x7780`
2. `ESP_ERR_HTTP_CONNECT`

These usually indicate temporary network/TLS handshake issues, not a permanent auth/config failure.
Because backlog is enabled, data is still preserved and retried later.

## Quick Start

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Fill Wi-Fi + Supabase values.
3. Ensure Supabase tables exist and names match `include/app_config.h`.
4. Build and flash.
5. Watch monitor logs for healthy startup and publish/flush behavior.

## Hardware Target

Current pin/config defaults are in `include/app_config.h`:

1. ESP32-C5 board (tested on 16 MB flash variant).
2. Status GPIO LED: `GPIO2`.
3. RGB (addressable LED data): `GPIO27`.
4. Piezo buzzer (LEDC PWM): `GPIO10`.
5. I2C SDA: `GPIO4`.
6. I2C SCL: `GPIO5`.
7. Battery ADC input: `GPIO0`.

## Project Layout

1. `src/main.cpp`: bootstrap, retry logic, log-level tuning.
2. `src/rtu_controller.h/.cpp`: core orchestration, task creation, state machine.
3. `src/sensor_hal.h/.cpp`: BME280 + battery ADC read path.
4. `src/network_hal.h/.cpp`: Wi-Fi events + Supabase REST client.
5. `src/actuator_hal.h/.cpp`: RGB + buzzer control.
6. `src/backlog_store.h/.cpp`: SPIFFS-backed NDJSON queue.
7. `include/app_config.h`: all tunable compile-time config.
8. `include/secrets.example.h`: safe credential template.
9. `partitions.csv`: app + SPIFFS partition sizing.

## Configuration Details

### Secrets

Set in `include/secrets.h`:

1. `WIFI_SSID`
2. `WIFI_PASS`
3. `SUPABASE_URL` format: `https://<project-ref>.supabase.co`
4. `SUPABASE_API_KEY`: use publishable/anon-style key for current firmware model

Do not put Supabase secret/service-role key directly in firmware.

### App behavior and timing

Set in `include/app_config.h`:

1. `DEFAULT_SAMPLING_INTERVAL_MS = 5000` (default sample period = 5 seconds).
2. `MIN_SAMPLING_INTERVAL_MS = 1000`.
3. `MAX_SAMPLING_INTERVAL_MS = 600000`.
4. `COMMAND_POLL_INTERVAL_MS = 5000`.
5. `MAX_BACKLOG_LINES = 2000`.

### Supabase table names

Defaults in `include/app_config.h`:

1. `TABLE_TELEMETRY = "telemetry"`
2. `TABLE_COMMANDS = "commands"`
3. `TABLE_STATUS = "status"`

These names must match your actual Supabase tables exactly.

## Supabase Table Expectations

At minimum:

1. `telemetry` table: accepts JSON fields emitted by `sampleToJson`.
2. `status` table: accepts fields from `statusToJson`.
3. `commands` table: expected by polling path.

`commands` should include at least:

1. `id` (monotonic key used for ordering/patch).
2. `device_id` (text).
3. `command` (json/jsonb payload).
4. `processed` (boolean).

Firmware command poll query expects:

1. `device_id = eq.<DEVICE_ID>`
2. `processed = eq.false`
3. `order = id.asc`
4. `limit = 1`
5. `select = id,command`

## Build and Flash

From an ESP-IDF terminal:

```bash
idf.py set-target esp32c5
idf.py fullclean
idf.py build
idf.py -p COM12 flash monitor
```

If `idf.py` is not found, open the command prompt from your ESP-IDF environment first.

## Partition Table

`partitions.csv` uses:

1. `factory` app partition: `0x600000` (6 MB)
2. `spiffs` data partition: `0x200000` (2 MB)

This is why your large binary builds while still keeping local backlog storage.

## Boot and Runtime Sequence

1. Bootloader loads app.
2. `main.cpp` starts RTU bootstrap with retry.
3. RTU initializes actuator, sensor, network, backlog.
4. RTU spawns `sampleTask`, `publishTask`, `connectivityTask`.
5. System enters steady-state loop.

Healthy startup markers:

1. `NetworkHal: Wi-Fi connected. IP address acquired: ...`
2. `RTU controller started (sampling, publish, and connectivity tasks active)`
3. repeating `Telemetry sent` and/or `Flushed backlog (...)`

## Troubleshooting Guide

### Symptom: `This authentication method is not supported: Bearer`

Likely causes:

1. Wrong key type or malformed auth header usage.
2. Old firmware binary still running.

Actions:

1. Verify current `include/secrets.h`.
2. `idf.py fullclean build flash`.
3. Confirm monitor startup compile timestamp changed.

### Symptom: HTTP `404` on telemetry/commands/status

Cause:

1. Table names in cloud do not match `app_config.h`.

Action:

1. Align table names or update constants.

### Symptom: occasional `mbedtls_ssl_handshake returned -0x7780`

Cause:

1. Transient TLS/network handshake interruption.

Action:

1. Usually no action required unless persistent.
2. Backlog should protect data continuity.
3. Check AP stability and signal quality if frequent.

### Symptom: BME280 not found

Log:

1. `BME280 not detected; using fallback simulated sensor values`

Cause:

1. Sensor not wired/addressed correctly, or absent.

Action:

1. Verify I2C wiring, pull-ups, device address.
2. Sensor fallback is expected until fixed.

### Symptom: monitor disconnect (`ClearCommError`)

Cause:

1. Host serial/USB tool issue, board reset, cable glitch.

Action:

1. Re-open monitor.
2. Check cable/port stability.
3. Firmware itself may still be running.

## Current Known Limitations

1. Sensor HAL currently keeps legacy I2C driver include path (`driver/i2c.h` warning).
2. Sensor fallback mode can hide missing hardware unless logs are monitored.
3. Security hardening is intentionally deferred while bring-up remains active.

## Security Hardening Roadmap

See [NEXT_STEPS.md](/C:/Users/Ahmad/Desktop/Tersano_ESP_Challenge/NEXT_STEPS.md) for deferred items:

1. per-device auth identity
2. stricter RLS model
3. key rotation and policy tightening
