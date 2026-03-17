-- 003_telemetry_reboot_safe_capture_time.sql
-- Purpose:
--   Add reboot-safe capture lineage fields and a resolved-timestamp view for
--   telemetry rows that may be replayed from backlog after a reboot.

begin;

do $$
begin
  if to_regclass('public.telemetry') is null then
    raise notice 'public.telemetry not found; skipping reboot-safe telemetry migration';
    return;
  end if;

  alter table public.telemetry
    add column if not exists captured_boot_id bigint;

  alter table public.telemetry
    add column if not exists published_boot_id bigint;

  alter table public.telemetry
    add column if not exists captured_unix_ms bigint;
end $$;

create index if not exists idx_telemetry_device_created_at_desc
  on public.telemetry (device_id, created_at desc);

create index if not exists idx_telemetry_device_captured_unix_ms
  on public.telemetry (device_id, captured_unix_ms)
  where captured_unix_ms is not null;

create index if not exists idx_telemetry_device_boot_ids_created_at
  on public.telemetry (device_id, captured_boot_id, published_boot_id, created_at desc);

create or replace view public.telemetry_resolved as
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

grant select on public.telemetry_resolved to anon, authenticated;

commit;
