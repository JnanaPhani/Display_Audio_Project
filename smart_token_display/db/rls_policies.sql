-- ============================================================================
--  Smart Token Display — RLS fix for the ESP32 (anon key) firmware
-- ----------------------------------------------------------------------------
--  Symptom this fixes:
--    HTTP 401 / "new row violates row-level security policy"
--      for table "token_events" and "device_status"
--    (seen on the device as: token_event HTTP 401 / Heartbeat 401)
--
--  Run this whole file once in the Supabase SQL editor.
--  It is idempotent — safe to re-run. It does NOT create/alter your existing
--  device_status table or its trigger; it only adjusts permissions/policies.
--
--  Strategy:
--    * record_token_event RPC -> SECURITY DEFINER, so it writes to
--      token_events / token_status with the function owner's rights and is
--      NOT blocked by RLS. The anon role only needs EXECUTE on it.
--    * device_status          -> the firmware upserts this table directly,
--      so it needs explicit RLS INSERT/UPDATE/SELECT policies for anon.
-- ============================================================================


-- ----------------------------------------------------------------------------
-- 1. Make the token-event RPC bypass RLS safely (SECURITY DEFINER).
-- ----------------------------------------------------------------------------
alter function public.record_token_event(text, text, text, text, date, text)
    security definer;

-- Pin search_path so a SECURITY DEFINER function can't be hijacked.
alter function public.record_token_event(text, text, text, text, date, text)
    set search_path = public, pg_temp;

-- Ensure the firmware's role can call it.
grant execute on function public.record_token_event(text, text, text, text, date, text) to anon;
grant execute on function public.record_token_event(text, text, text, text, date, text) to authenticated;


-- ----------------------------------------------------------------------------
-- 2a. FIX: device_status trigger references a non-existent column.
--     The trigger trg_device_status_updated_at calls
--     touch_device_status_updated_at(), which sets NEW.updated_at — but this
--     table has NO updated_at column (it has last_seen). Every insert/update
--     therefore fails with:
--         42703: record "new" has no field "updated_at"
--     which is why device_status was empty (the firmware logged "Heartbeat
--     sent" because it only checks the transport result, not the HTTP status).
--     Re-point the function at the column that actually exists: last_seen.
-- ----------------------------------------------------------------------------
create or replace function public.touch_device_status_updated_at()
returns trigger
language plpgsql
as $$
begin
    new.last_seen := now();
    return new;
end;
$$;


-- ----------------------------------------------------------------------------
-- 2. device_status — enable RLS and allow anon to upsert + read.
--    (Table + trigger already exist in your DB; we only touch policies/grants.)
--
--    Schema note: this table has a `wifi_password` column. The firmware never
--    needs to read it back, so we deliberately do NOT grant SELECT on it to the
--    anon role — preventing anyone holding the (flash-embedded) anon key from
--    dumping stored Wi-Fi passwords. SELECT is still granted on every other
--    column because the realtime UPDATE subscription (reprovision trigger)
--    needs to receive row data.
-- ----------------------------------------------------------------------------
alter table public.device_status enable row level security;

drop policy if exists "anon insert device_status" on public.device_status;
create policy "anon insert device_status"
    on public.device_status for insert to anon
    with check (true);

drop policy if exists "anon update device_status" on public.device_status;
create policy "anon update device_status"
    on public.device_status for update to anon
    using (true) with check (true);

drop policy if exists "anon select device_status" on public.device_status;
create policy "anon select device_status"
    on public.device_status for select to anon
    using (true);

-- Table-level grants. Start clean, then grant INSERT/UPDATE on all columns and
-- SELECT on every column EXCEPT wifi_password.
revoke all on public.device_status from anon;
grant insert, update on public.device_status to anon;
grant select (
    device_id, last_seen, wifi_rssi, uptime_seconds, firmware_version,
    last_reboot_reason, device_timestamp, wifi_ssid,
    last_provisioned_at, reprovision_trigger, brownout_count, brightness,
    device_type, volume, name
) on public.device_status to anon;

-- authenticated (e.g. your dashboard/service) keeps full access.
grant select, insert, update on public.device_status to authenticated;


-- ----------------------------------------------------------------------------
-- 3. token_events / token_status — RLS is bypassed by the SECURITY DEFINER
--    RPC above, so no anon WRITE policy is required.
--
--    Reads, however, are still governed by RLS. With RLS enabled and no SELECT
--    policy, the Supabase dashboard / a logged-in app sees ZERO rows (even
--    though the device writes are landing). The policies below let the
--    `authenticated` role (your dashboard / signed-in users) read the data.
--
--    NOTE: we intentionally do NOT grant SELECT to `anon` here — the anon key
--    is embedded in device flash, so anon-readable = publicly readable.
-- ----------------------------------------------------------------------------
alter table public.token_events enable row level security;
alter table public.token_status enable row level security;

drop policy if exists "authenticated read token_events" on public.token_events;
create policy "authenticated read token_events"
    on public.token_events for select to authenticated
    using (true);

drop policy if exists "authenticated read token_status" on public.token_status;
create policy "authenticated read token_status"
    on public.token_status for select to authenticated
    using (true);

grant select on public.token_events to authenticated;
grant select on public.token_status to authenticated;


-- ============================================================================
--  After running:
--   * Re-scan a token on the device  -> "token_event ok" / "Heartbeat sent".
--   * To SEE the rows yourself, either run in the SQL editor (bypasses RLS):
--         select * from public.token_events order by scanned_at desc;
--         select * from public.token_status order by last_event_at desc;
--     or view them in a signed-in dashboard (authenticated role) via the
--     read policies added in section 3.
-- ============================================================================
