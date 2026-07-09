-- ============================================================================
--  Smart Token Display — OTA status, crash logs, and canary rollout
-- ----------------------------------------------------------------------------
--  Run once in the Supabase SQL editor. Idempotent — safe to re-run.
--
--  Adds:
--    1. system_control.target_device_id  — canary targeting (NULL = whole fleet,
--       a MAC = only that device updates).
--    2. ota_status   table — every OTA outcome reported by devices.
--    3. crash_logs   table — crash backtrace summaries reported on next boot.
--    4. RPCs (SECURITY DEFINER) so the anon firmware key can insert into the two
--       tables without being blocked by RLS — same pattern as record_token_event.
-- ============================================================================


-- ----------------------------------------------------------------------------
-- 1. Canary targeting column on system_control.
--    NULL  -> trigger applies to the whole fleet (current behaviour).
--    "AA:BB:CC:DD:EE:FF" -> only the device whose STA MAC matches updates.
-- ----------------------------------------------------------------------------
alter table public.system_control
    add column if not exists target_device_id text null;
alter table public.system_control
    add column if not exists device_type text null;


-- ----------------------------------------------------------------------------
-- 2. ota_status — one row per OTA attempt/outcome reported by a device.
--    phase: 'failed' | 'applied' | 'success' | 'degraded'
--      failed   = download/apply error (with err detail + attempt #)
--      applied  = image written OK, about to reboot into pending-verify
--      success  = new image booted AND passed self-test (peripherals+wifi+cloud)
--      degraded = new image booted, kept (no rollback), but cloud unreachable
-- ----------------------------------------------------------------------------
create table if not exists public.ota_status (
    id            bigint generated always as identity primary key,
    device_id     text        not null,
    from_version  text,
    to_version    text,
    phase         text        not null
                    check (phase in ('failed','applied','success','degraded')),
    error         text,                       -- esp_err name / class on failure
    attempt       integer,                    -- retry attempt number on failure
    reported_at   timestamptz not null default now()
);

create index if not exists ota_status_device_idx on public.ota_status (device_id);
create index if not exists ota_status_time_idx   on public.ota_status (reported_at desc);


-- ----------------------------------------------------------------------------
-- 3. crash_logs — crash summary uploaded on the boot AFTER a crash.
-- ----------------------------------------------------------------------------
create table if not exists public.crash_logs (
    id                   bigint generated always as identity primary key,
    device_id            text        not null,
    firmware_version     text,
    reboot_reason        text,                -- esp_reset_reason mapped string
    crashed_task         text,                -- task name from coredump summary
    fault_pc             text,                -- program counter (hex string)
    backtrace            text,                -- space-separated addresses (hex)
    uptime_before_crash  bigint,              -- seconds, if known
    reported_at          timestamptz not null default now()
);

create index if not exists crash_logs_device_idx on public.crash_logs (device_id);
create index if not exists crash_logs_time_idx   on public.crash_logs (reported_at desc);


-- ----------------------------------------------------------------------------
-- 4. Insert RPCs (SECURITY DEFINER) — let the anon firmware key write without
--    being blocked by RLS, exactly like record_token_event.
-- ----------------------------------------------------------------------------
create or replace function public.report_ota_status(
    p_device_id    text,
    p_from_version text,
    p_to_version   text,
    p_phase        text,
    p_error        text default null,
    p_attempt      integer default null
)
returns bigint
language plpgsql
security definer
set search_path = public, pg_temp
as $$
declare v_id bigint;
begin
    insert into public.ota_status
        (device_id, from_version, to_version, phase, error, attempt)
    values
        (p_device_id, p_from_version, p_to_version, p_phase, p_error, p_attempt)
    returning id into v_id;
    return v_id;
end $$;

create or replace function public.report_crash(
    p_device_id           text,
    p_firmware_version    text,
    p_reboot_reason       text,
    p_crashed_task        text  default null,
    p_fault_pc            text  default null,
    p_backtrace           text  default null,
    p_uptime_before_crash bigint default null
)
returns bigint
language plpgsql
security definer
set search_path = public, pg_temp
as $$
declare v_id bigint;
begin
    insert into public.crash_logs
        (device_id, firmware_version, reboot_reason, crashed_task,
         fault_pc, backtrace, uptime_before_crash)
    values
        (p_device_id, p_firmware_version, p_reboot_reason, p_crashed_task,
         p_fault_pc, p_backtrace, p_uptime_before_crash)
    returning id into v_id;
    return v_id;
end $$;

grant execute on function public.report_ota_status(text,text,text,text,text,integer) to anon;
grant execute on function public.report_ota_status(text,text,text,text,text,integer) to authenticated;
grant execute on function public.report_crash(text,text,text,text,text,text,bigint) to anon;
grant execute on function public.report_crash(text,text,text,text,text,text,bigint) to authenticated;


-- ----------------------------------------------------------------------------
-- 5. RLS: enable on both tables, allow signed-in dashboard reads. Writes go
--    only through the SECURITY DEFINER RPCs above (no anon write policy needed).
-- ----------------------------------------------------------------------------
alter table public.ota_status enable row level security;
alter table public.crash_logs enable row level security;

drop policy if exists "authenticated read ota_status" on public.ota_status;
create policy "authenticated read ota_status"
    on public.ota_status for select to authenticated using (true);

drop policy if exists "authenticated read crash_logs" on public.crash_logs;
create policy "authenticated read crash_logs"
    on public.crash_logs for select to authenticated using (true);

grant select on public.ota_status to authenticated;
grant select on public.crash_logs to authenticated;


-- ============================================================================
--  Canary release flow (used by release.py):
--    1. Publish vX with target_device_id = <canary MAC>  -> only that device updates.
--    2. Watch:  select * from ota_status where to_version = 'vX' order by reported_at;
--       Wait for phase='success' from the canary.
--    3. Set target_device_id = NULL  -> the rest of the fleet updates.
--
--  Inspect crashes:
--    select * from crash_logs order by reported_at desc;
-- ============================================================================
