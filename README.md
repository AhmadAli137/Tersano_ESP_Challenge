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

Think of the firmware as four loops running in parallel:

1. `sampleTask`: produce samples on a fixed interval.
2. `publishTask`: send fresh sample immediately; if it fails, cache it.
3. `connectivityTask`: maintain status/health state and drain old cached rows.
4. `commandTask`: poll pending cloud commands and dispatch command handlers.

This gives you both low latency (fresh sample send) and reliability (persistent backlog).
It also isolates command polling latency/failures from backlog replay and live telemetry publishing.

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
3. Magenta: connected + cloud healthy + sampling interval set to `5000 ms` (5 seconds, Fast).
4. Green: connected + cloud healthy + sampling interval set to `300000 ms` (5 minutes, Default).
5. Blue: connected + cloud healthy + sampling interval set to `1800000 ms` (30 minutes, Slow).

Blink behavior:

1. `toggle_led_blink` command can enable/disable LED blinking.
2. Blinking applies to whatever status color is currently active.
3. Blink cadence is 1 second period, 50% duty.

## Intermittent Publish Failures (Why They Happen)

You may still see occasional lines like:

1. `mbedtls_ssl_handshake returned -0x7780`
2. `ESP_ERR_HTTP_CONNECT`

These usually indicate temporary network/TLS handshake issues, not a permanent auth/config failure.
Because backlog is enabled, data is still preserved and retried later.

## Quick Start

1. Copy `include/secrets.example.h` to `include/secrets.h`.
2. Fill Wi-Fi + Supabase values and set `DEVICE_ID`.
3. Use a unique `DEVICE_ID` per physical board before each flash/provision.
4. Ensure Supabase tables exist and names match `include/app_config.h`.
5. Build and flash.
6. Watch monitor logs for healthy startup and publish/flush behavior.

## Hardware Target

Current pin/config defaults are in `include/app_config.h`:

1. ESP32-C5 board (tested on 16 MB flash variant).
2. Status GPIO LED: `GPIO24`.
3. RGB (addressable LED data): `GPIO27`.
4. Piezo buzzer (LEDC PWM): `GPIO26`.
5. I2C SDA: `GPIO2`.
6. I2C SCL: `GPIO3`.
7. Battery ADC input: `GPIO6` (`ADC1_CH5`).

Reference board spec used for this project:

1. ESP32-C5 Dual-Band WFi 6 Development Board
2. 240MHz RISC-V Processor,Onboard ESP32-C5-WROOM-1 Series Module, Multi-Protocol RISC-V MCU, 16 MB Flash and 8 MB PS-RAM

## Project Layout

1. `src/main.cpp`: bootstrap, retry logic, log-level tuning.
2. `src/rtu_controller.h/.cpp`: core orchestration, task creation, state machine.
3. `src/sensor_hal.h/.cpp`: BME280 + battery ADC read path.
4. `src/network_hal.h/.cpp`: Wi-Fi events + Supabase REST transport + command polling.
5. `src/actuator_hal.h/.cpp`: RGB + buzzer control.
6. `src/backlog_store.h/.cpp`: SPIFFS-backed NDJSON queue with trim utilities.
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
5. `DEVICE_ID`: required runtime identity for telemetry/status rows and command targeting

Do not put Supabase secret/service-role key directly in firmware.

`DEVICE_ID` guidance:

1. Keep `DEVICE_ID` stable for the same board across reflashes to preserve history continuity.
2. Use a different `DEVICE_ID` for each board to avoid mixed telemetry/commands.
3. If left empty, firmware falls back to MAC-derived identity `rtu-esp32c5-<MAC_LAST_3_BYTES_HEX>` and logs a warning.

### App behavior and timing

Set in `include/app_config.h`:

1. `DEFAULT_SAMPLING_INTERVAL_MS = 300000` (default sample period = 5 minutes).
2. `MIN_SAMPLING_INTERVAL_MS = 5000` (fast preset floor: 5 seconds).
3. `MAX_SAMPLING_INTERVAL_MS = 1800000` (slow preset ceiling: 30 minutes).
4. `COMMAND_POLL_INTERVAL_MS = 1000` (base cadence; adaptive idle backoff in `NetworkHal` may increase it temporarily).
5. `MAX_BACKLOG_LINES = 2000`.

### Supabase table names

Defaults in `include/app_config.h`:

1. `TABLE_TELEMETRY = "telemetry"`
2. `TABLE_COMMANDS = "device_commands"`
3. `TABLE_STATUS = "status"`

These names must match your actual Supabase tables exactly.

## Supabase Table Expectations

At minimum:

1. `telemetry` table: accepts JSON fields emitted by `sampleToJson`.
2. `status` table: accepts fields from `statusToJson`.
3. `device_commands` table: expected by polling path.

Telemetry lineage fields emitted by firmware:

1. `captured_uptime_ms`: uptime when sample was taken on device.
2. `published_uptime_ms`: uptime when sample was actually sent to cloud.
3. `captured_boot_id`: persistent boot session counter at capture time.
4. `published_boot_id`: persistent boot session counter at publish time.
5. `captured_unix_ms`: device wall-clock timestamp at capture time when available; `null` when unsynchronized.
6. `was_cached`: `true` when row was replayed from backlog, else `false`.

### Telemetry timestamp semantics (important)

`telemetry.created_at` is assigned by Supabase when the row is inserted, which means:

1. For live rows, `created_at` is near the actual sensor capture time.
2. For backlog rows, `created_at` reflects replay time, not original capture time.

Uptime lineage fields let you recover capture timing when the device has not rebooted between capture and publish:

1. Capture timestamp estimate formula:
   - `estimated_capture_time = created_at - (published_uptime_ms - captured_uptime_ms)`
2. This formula is valid only when:
   - `captured_boot_id = published_boot_id`
   - `published_uptime_ms >= captured_uptime_ms`

If those conditions fail, the row crossed a reboot boundary, so uptime subtraction is invalid and you should treat capture time as uncertain.

`device_commands` should include at least:

1. `id` (monotonic key used for ordering/patch).
2. `device_id` (text).
3. `command` (json/jsonb payload).
4. `processed` (boolean).

Firmware command poll query expects:

1. `device_id = eq.<runtime device id>`
2. `processed = eq.false`
3. `order = id.asc`
4. `limit = 1`
5. `select = *` (firmware accepts `command` or fallback `payload` JSON fields)

### Device ID behavior

Device ID is explicitly assigned per build:

1. Set `secrets::DEVICE_ID` in `include/secrets.h` before flashing each board.
2. If left empty, firmware uses fallback `rtu-esp32c5-<MAC_LAST_3_BYTES_HEX>` and logs a warning.

This means you do not map IDs to COM ports. Identity is an explicit provisioning choice.

Supported command payloads (in `device_commands.command` JSON):

1. `{"type":"set_sampling_interval","sampling_interval_ms":300000}`
2. `{"type":"play_buzzer","frequency":1500,"duration":300}`
3. `{"type":"toggle_led_blink","enabled":true}`

Notes:

1. `set_sampling_interval` drives the status LED color map for healthy/connected state.
2. `play_buzzer` is a momentary tone pulse and then returns to off.
3. `toggle_led_blink` accepts `enabled` (also `on`/`blink` aliases for compatibility).

Command outcome events written to `status`:

1. `command_applied` with metadata `{ "result":"pass", "type":"..." }`
2. `command_failed` with metadata `{ "result":"fail", "reason":"...", "type":"..." }`

### Status table migration (recommended)

Run the included SQL migration to create/align the `status` table used for device event rows:

1. Open Supabase Dashboard -> SQL Editor.
2. Paste and run:
   - [`supabase/migrations/001_status_events.sql`](/C:/Users/Ahmad/Desktop/Tersano_ESP_Challenge/supabase/migrations/001_status_events.sql)
3. Confirm inserts succeed for events like:
   - `boot`, `rtu_started`, `online`, `offline`, `calibrated`, `heartbeat`, `data_sync`, `cloud_degraded`, `cloud_recovered`, `command_applied`

Example metadata payloads now emitted by firmware:

1. `boot`: `{"firmware":"<project_ver>","reason":"power_on","boot_id":123}`
2. `online`: `{"ip":"192.168.x.x","rssi":-42}`
3. `calibrated`: `{"sensors":["temp","humidity","pressure"]}`
4. `heartbeat`: `{"status":"healthy|degraded"}` (emitted every 30s)
5. `data_sync`: `{"records":24,"duration_ms":1250}`

Status rows also include top-level runtime state fields:

1. `sample_rate_ms` (current sampling interval in milliseconds)
2. `blink_on` (current LED blink state)

### Telemetry table lineage migration (recommended)

Run the telemetry migration to keep everything in the `telemetry` table only (no extra table/view required):

1. Open Supabase Dashboard -> SQL Editor.
2. Paste and run:
   - [`supabase/migrations/002_telemetry_backlog_fields.sql`](/C:/Users/Ahmad/Desktop/Tersano_ESP_Challenge/supabase/migrations/002_telemetry_backlog_fields.sql)
3. Then run your telemetry-only SQL patch that adds:
   - `captured_boot_id`
   - `published_boot_id`
   - `captured_unix_ms`
4. Backfill legacy rows:
   - `captured_uptime_ms = uptime_ms` where missing.

### Reboot-safe query pattern (telemetry table only)

Use this select pattern in dashboards/API queries to compute best-effort capture time directly from `public.telemetry`:

```sql
select
  t.*,
  case
    when t.captured_unix_ms is not null
      and t.captured_unix_ms >= 1704067200000
    then to_timestamp(t.captured_unix_ms / 1000.0)
    when t.captured_boot_id is not null
      and t.published_boot_id is not null
      and t.captured_boot_id = t.published_boot_id
      and t.captured_uptime_ms is not null
      and t.published_uptime_ms is not null
      and t.published_uptime_ms >= t.captured_uptime_ms
    then t.created_at - ((t.published_uptime_ms - t.captured_uptime_ms) * interval '1 millisecond')
    else t.created_at
  end as resolved_capture_at,
  case
    when t.captured_unix_ms is not null
      and t.captured_unix_ms >= 1704067200000
    then 'captured_unix_ms'
    when t.captured_boot_id is not null
      and t.published_boot_id is not null
      and t.captured_boot_id = t.published_boot_id
      and t.captured_uptime_ms is not null
      and t.published_uptime_ms is not null
      and t.published_uptime_ms >= t.captured_uptime_ms
    then 'uptime_same_boot'
    else 'created_at_fallback'
  end as capture_time_source,
  case
    when t.captured_unix_ms is not null
      and t.captured_unix_ms >= 1704067200000
    then false
    when t.captured_boot_id is not null
      and t.published_boot_id is not null
      and t.captured_boot_id = t.published_boot_id
      and t.captured_uptime_ms is not null
      and t.published_uptime_ms is not null
      and t.published_uptime_ms >= t.captured_uptime_ms
    then false
    else true
  end as capture_time_uncertain
from public.telemetry t;
```

Interpretation:

1. `capture_time_source='captured_unix_ms'`: best quality.
2. `capture_time_source='uptime_same_boot'`: good quality, derived from uptime delta.
3. `capture_time_source='created_at_fallback'`: uncertain capture timing; treat as publish-time proxy.

### Why reboot-safe lineage works

Without boot IDs, a reboot can produce negative uptime deltas:

1. Example: `captured_uptime_ms=16,901,782`, `published_uptime_ms=73,872`.
2. Naive subtraction gives `-16,827,910 ms`, which pushes computed capture time into the future.

With boot IDs:

1. Firmware stamps `captured_boot_id` at sample time.
2. Firmware stamps `published_boot_id` at send time.
3. If IDs differ, uptime subtraction is skipped, and query falls back to safe logic.

### Supabase validation checklist

After deploying firmware + SQL, verify in SQL Editor:

```sql
-- Confirm columns exist
select column_name, data_type
from information_schema.columns
where table_schema='public' and table_name='telemetry'
  and column_name in (
    'captured_uptime_ms',
    'published_uptime_ms',
    'was_cached',
    'captured_boot_id',
    'published_boot_id',
    'captured_unix_ms'
  )
order by column_name;
```

```sql
-- Confirm new firmware rows are populating lineage fields
select
  device_id,
  created_at,
  captured_uptime_ms,
  published_uptime_ms,
  captured_boot_id,
  published_boot_id,
  captured_unix_ms,
  was_cached
from public.telemetry
order by created_at desc
limit 20;
```

```sql
-- Count uncertain rows (usually reboot-crossing or no valid wall-clock)
select
  count(*) as uncertain_rows
from public.telemetry t
where not (
  (t.captured_unix_ms is not null and t.captured_unix_ms >= 1704067200000)
  or
  (
    t.captured_boot_id is not null
    and t.published_boot_id is not null
    and t.captured_boot_id = t.published_boot_id
    and t.captured_uptime_ms is not null
    and t.published_uptime_ms is not null
    and t.published_uptime_ms >= t.captured_uptime_ms
  )
);
```

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

1. `factory` app partition: `0x280000` (2.5 MB)
2. `spiffs` data partition: `0x170000` (1.5 MB)

This layout is 4MB-flash safe and also works on larger-flash boards.

## Boot and Runtime Sequence

1. Bootloader loads app.
2. `main.cpp` starts RTU bootstrap with retry.
3. RTU initializes actuator, sensor, network, backlog.
4. RTU spawns `sampleTask`, `publishTask`, `connectivityTask`, and `commandTask`.
5. System enters steady-state loop.

Healthy startup markers:

1. `NetworkHal: Wi-Fi connected. IP address acquired: ...`
2. `RTU controller started (sampling, publish, connectivity, and command tasks active)`
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

1. `BME280 not detected; environmental fields will be reported as N/A`

Cause:

1. Sensor not wired/addressed correctly, or absent.

Action:

1. Verify I2C wiring, pull-ups, device address.
2. Environmental telemetry fields will be `null`/`N/A` until fixed.

### Symptom: monitor disconnect (`ClearCommError`)

Cause:

1. Host serial/USB tool issue, board reset, cable glitch.

Action:

1. Re-open monitor.
2. Check cable/port stability.
3. Firmware itself may still be running.

## Current Known Limitations

1. Sensor HAL currently keeps legacy I2C driver include path (`driver/i2c.h` warning).
2. Missing BME280 reports `N/A` environmental fields and `sensor_ok=false`.
3. Security hardening is intentionally deferred while bring-up remains active.

## Security Hardening Roadmap

See [NEXT_STEPS.md](/C:/Users/Ahmad/Desktop/Tersano_ESP_Challenge/NEXT_STEPS.md) for deferred items:

1. per-device auth identity
2. stricter RLS model
3. key rotation and policy tightening
