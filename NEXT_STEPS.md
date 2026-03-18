# Next Steps

## Recently Completed

1. Persisted runtime command settings to NVS:
   - sampling interval (`sample_interval_ms`)
   - LED blink enable state (`blink_on`)
2. Added restore-on-boot behavior so both settings survive reboot.

## Remaining Firmware Alignment

1. Add challenge-compatible command alias support for:
   - `{"action":"set_interval","value":30}` (seconds-based shape)
2. Decide and lock target default sampling cadence for demo expectations.
3. Optional bonus hardening:
   - explicit TWDT task integration
   - deep sleep between sample windows

## Deferred Security Hardening (Supabase)

These items are intentionally deferred for now so development can continue.

1. Move from permissive `anon` access to per-device authentication.
2. Issue unique credentials/token per physical device (not shared across fleet).
3. Update RLS policies to enforce identity from auth claims, not request payload fields.
4. Restrict table access so each device can only:
   - insert its own telemetry/status rows
   - read/update only its own command rows
5. Rotate any exposed keys and remove old credentials from active use.

## Why This Matters

With only `anon` key + `device_id` checks, requests can still be spoofed by a client that knows project URL/key and sends the same `device_id`.

## Done Criteria (Security Phase)

1. Device requests use per-device auth token/credential.
2. RLS policies use auth identity (`auth.uid()`/claims) as source of truth.
3. Cross-device reads/writes are denied by policy tests.
4. Old permissive policies are removed.

## Device ID Assignment SOP (Professional Workflow)

Goal: make board identity explicit and operator-controlled at build/flash time.

### Policy

1. `DEVICE_ID` is assigned by the programmer before each flash.
2. Never derive identity from COM port.
3. Never reuse a retired `DEVICE_ID` for a different physical unit.
4. Keep a single source-of-truth registry that maps:
   - device_id
   - board serial label/sticker
   - chip MAC
   - hardware revision
   - first provisioned date
   - operator initials

### Recommended ID format

1. Use stable human-readable IDs, e.g. `rtu-esp32c5-01`, `rtu-esp32c5-02`.
2. Keep width fixed (`01`, `02`, ... `99`) for sorting and dashboards.
3. Avoid embedding temporary info (COM port, desk number, operator name).

### Provisioning checklist (per board)

1. Connect only one board to avoid port confusion.
2. Set `DEVICE_ID` in `include/secrets.h`.
3. Build and flash.
4. On boot, verify serial log:
   - `Base MAC: ...`
   - `Device identity: <expected DEVICE_ID>`
5. Confirm in Supabase `status` table that new rows use expected `device_id`.
6. Record/update registry entry with MAC + assigned ID.
7. Physically label board with assigned `DEVICE_ID`.

### Change control

1. If a board is replaced, assign a new `DEVICE_ID` unless you intentionally preserve identity.
2. If preserving identity, document replacement event in registry history.
3. For accidental wrong assignment:
   - fix `DEVICE_ID` in `secrets.h`
   - reflash immediately
   - annotate incident in registry notes.

### Team operations

1. Treat `include/secrets.h` as local provisioning config, not shared truth.
2. Keep registry in a shared controlled location (sheet, DB, or asset system).
3. Add a pre-flash peer check for production batches:
   - expected DEVICE_ID
   - board sticker
   - MAC observed at boot.

### Future hardening (optional)

1. Move identity out of source file into signed provisioning artifact.
2. Tie Supabase auth to per-device credentials and enforce identity in RLS claims.
3. Add automated manufacturing test that rejects duplicate IDs before release.
