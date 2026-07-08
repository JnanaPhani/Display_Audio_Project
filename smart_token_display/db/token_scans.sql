-- ============================================================================
--  Smart Token Display — token event schema
-- ----------------------------------------------------------------------------
--  Run this once against your Supabase / Postgres database (SQL editor or psql).
--
--  Barcode format scanned by the device:  BY-<YYYYMMDD>-T<token>
--    * Offline / walk-in : token is purely numeric   e.g. BY-20260615-T42   -> token "42",   shown on display
--    * Online order      : token has letters (ORD..) e.g. BY-20260615-TORD42 -> token "ORD42", never displayed
--
--  Event lifecycle (status values the firmware sends):
--    * online order, every scan        -> 'packing_complete'
--    * offline order, 1st scan         -> 'ready_to_collect'   (number shown on the P10 panel)
--    * offline order, 2nd scan         -> 'no_show'            (customer didn't collect; number removed from panel)
--
--  The ESP32 firmware calls:
--      POST /rest/v1/rpc/record_token_event
--      body: { "p_order_id":"BY-20260615-T42", "p_token":"42",
--              "p_order_type":"offline", "p_status":"ready_to_collect",
--              "p_event_date":"2026-06-15", "p_device_id":"AA:BB:.." }
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 0. Enum-like check domains (kept as text + CHECK for simplicity/portability)
-- ----------------------------------------------------------------------------
--   order_type : 'online' | 'offline'
--   status     : 'packing_complete' | 'ready_to_collect' | 'no_show'

-- ----------------------------------------------------------------------------
-- 1. Append-only event log: every scan that the device acts on = 1 row.
-- ----------------------------------------------------------------------------
create table if not exists public.token_events (
    id          bigint generated always as identity primary key,
    order_id    text        not null,                 -- full scanned barcode, e.g. BY-20260615-T42
    token       text        not null,                 -- decoded token: "42" or "ORD42"
    order_type  text        not null
                  check (order_type in ('online', 'offline')),
    status      text        not null
                  check (status in ('packing_complete', 'ready_to_collect', 'no_show')),
    device_id   text        not null,                 -- which ESP32 (MAC) produced the event
    event_date  date,                                 -- date decoded from the barcode (YYYYMMDD)
    scanned_at  timestamptz not null default now()    -- server receive time
);

create index if not exists token_events_order_id_idx   on public.token_events (order_id);
create index if not exists token_events_device_id_idx   on public.token_events (device_id);
create index if not exists token_events_status_idx      on public.token_events (status);
create index if not exists token_events_event_date_idx  on public.token_events (event_date);
create index if not exists token_events_scanned_at_idx  on public.token_events (scanned_at desc);

-- ----------------------------------------------------------------------------
-- 2. Current status per order (upsert): one row per order_id with latest state.
--    Handy for dashboards: "what is the live status of every order today".
-- ----------------------------------------------------------------------------
create table if not exists public.token_status (
    order_id        text        primary key,
    token           text        not null,
    order_type      text        not null,
    status          text        not null,             -- latest status seen
    device_id       text,                             -- device that set the latest status
    event_date      date,
    first_seen_at   timestamptz not null default now(),
    last_event_at   timestamptz not null default now()
);

create index if not exists token_status_status_idx     on public.token_status (status);
create index if not exists token_status_event_date_idx on public.token_status (event_date);

-- ----------------------------------------------------------------------------
-- 3. Atomic "record a token event" RPC: append to log + upsert latest status.
--    Returns the event id (bigint) of the inserted history row.
-- ----------------------------------------------------------------------------
create or replace function public.record_token_event(
    p_order_id   text,
    p_token      text,
    p_order_type text,
    p_status     text,
    p_event_date date default null,
    p_device_id  text default null
)
returns bigint
language plpgsql
as $$
declare
    v_id bigint;
begin
    -- 1) Immutable history row.
    insert into public.token_events
        (order_id, token, order_type, status, device_id, event_date)
    values
        (p_order_id, p_token, p_order_type, p_status,
         coalesce(p_device_id, 'unknown'), p_event_date)
    returning id into v_id;

    -- 2) Upsert the "latest status" view for this order.
    insert into public.token_status as s
        (order_id, token, order_type, status, device_id, event_date, last_event_at)
    values
        (p_order_id, p_token, p_order_type, p_status, p_device_id, p_event_date, now())
    on conflict (order_id) do update
        set token        = excluded.token,
            order_type   = excluded.order_type,
            status       = excluded.status,
            device_id    = coalesce(excluded.device_id, s.device_id),
            event_date   = coalesce(excluded.event_date, s.event_date),
            last_event_at = now();

    return v_id;
end;
$$;

-- ----------------------------------------------------------------------------
-- 4. Permissions: allow the anon role (key the firmware uses) to call the RPC.
-- ----------------------------------------------------------------------------
grant execute on function public.record_token_event(text, text, text, text, date, text) to anon;
grant execute on function public.record_token_event(text, text, text, text, date, text) to authenticated;

-- Optional RLS (uncomment + adjust if you enable Row Level Security on the tables):
-- alter table public.token_events enable row level security;
-- alter table public.token_status enable row level security;
-- create policy "anon insert events" on public.token_events
--     for insert to anon with check (true);
-- create policy "anon upsert status" on public.token_status
--     for all to anon using (true) with check (true);
