create table if not exists public.device_status (
  device_id text not null,
  last_seen timestamp with time zone not null default now(),
  wifi_rssi integer null,
  uptime_seconds bigint null,
  firmware_version text null,
  last_reboot_reason text null,
  device_timestamp bigint null,
  wifi_ssid text null,
  wifi_password text null,
  last_provisioned_at timestamp with time zone null,
  reprovision_trigger integer null default 1,
  brownout_count integer null default 0,
  brightness integer null default 75,
  constraint device_status_pkey primary key (device_id)
) TABLESPACE pg_default;

create trigger trg_device_status_updated_at BEFORE INSERT
or
update on device_status for EACH row
execute FUNCTION touch_device_status_updated_at ();