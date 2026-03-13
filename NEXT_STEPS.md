# Next Steps

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
