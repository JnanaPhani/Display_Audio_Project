create table public.token_events (
  id bigint generated always as identity not null,
  order_id text not null,
  token text not null,
  order_type text not null,
  status text not null,
  device_id text not null,
  event_date date null,
  scanned_at timestamp with time zone not null default now(),
  constraint token_events_pkey primary key (id),
  constraint token_events_order_type_check check (
    (
      order_type = any (array['online'::text, 'offline'::text])
    )
  ),
  constraint token_events_status_check check (
    (
      status = any (
        array[
          'packing_complete'::text,
          'ready_to_collect'::text,
          'no_show'::text
        ]
      )
    )
  )
) TABLESPACE pg_default;

create index IF not exists token_events_order_id_idx on public.token_events using btree (order_id) TABLESPACE pg_default;

create index IF not exists token_events_device_id_idx on public.token_events using btree (device_id) TABLESPACE pg_default;

create index IF not exists token_events_status_idx on public.token_events using btree (status) TABLESPACE pg_default;

create index IF not exists token_events_event_date_idx on public.token_events using btree (event_date) TABLESPACE pg_default;

create index IF not exists token_events_scanned_at_idx on public.token_events using btree (scanned_at desc) TABLESPACE pg_default;