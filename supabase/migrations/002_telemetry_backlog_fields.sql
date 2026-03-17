-- 002_telemetry_backlog_fields.sql
-- Purpose:
--   Add publish/capture lineage fields to telemetry rows so dashboards can
--   distinguish live vs backlog-replayed samples and capture vs publish timing.

begin;

do $$
begin
  if to_regclass('public.telemetry') is null then
    raise notice 'public.telemetry not found; skipping telemetry lineage migration';
    return;
  end if;

  alter table public.telemetry
    add column if not exists captured_uptime_ms bigint;

  alter table public.telemetry
    add column if not exists published_uptime_ms bigint;

  alter table public.telemetry
    add column if not exists was_cached boolean not null default false;
end $$;

-- Backfill capture uptime from existing uptime_ms where available.
update public.telemetry
set captured_uptime_ms = uptime_ms
where captured_uptime_ms is null
  and uptime_ms is not null;

create index if not exists idx_telemetry_device_created_at
  on public.telemetry (device_id, created_at desc);

create index if not exists idx_telemetry_device_was_cached_created_at
  on public.telemetry (device_id, was_cached, created_at desc);

commit;

