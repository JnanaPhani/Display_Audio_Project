create table public.token_scan_counts (
  order_id text not null,
  scan_count integer not null default 0,
  display_num integer null,
  last_device_id text null,
  first_scanned_at timestamp with time zone not null default now(),
  last_scanned_at timestamp with time zone not null default now(),
  constraint token_scan_counts_pkey primary key (order_id)
) TABLESPACE pg_default;

create index IF not exists token_scan_counts_last_scanned_idx on public.token_scan_counts using btree (last_scanned_at desc) TABLESPACE pg_default;