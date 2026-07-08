create table public.token_status (
  order_id text not null,
  token text not null,
  order_type text not null,
  status text not null,
  device_id text null,
  event_date date null,
  first_seen_at timestamp with time zone not null default now(),
  last_event_at timestamp with time zone not null default now(),
  constraint token_status_pkey primary key (order_id)
) TABLESPACE pg_default;

create index IF not exists token_status_status_idx on public.token_status using btree (status) TABLESPACE pg_default;

create index IF not exists token_status_event_date_idx on public.token_status using btree (event_date) TABLESPACE pg_default;