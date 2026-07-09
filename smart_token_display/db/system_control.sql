create table public.system_control (
  id bigint not null default 1,
  version text not null,
  bin_url text not null,
  update_description text null,
  updated_at timestamp with time zone null default now(),
  device_type text null,
  constraint system_control_pkey primary key (id)
) TABLESPACE pg_default;