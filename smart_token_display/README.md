# Smart Token Display

Offline-first ESP32-S3 firmware for a shop "token / order ready" system. A USB
barcode scanner is plugged into each device; scanning a customer's token shows
that number on a red **P10 LED matrix** so customers know their order is ready.
Every scan is also recorded in a **Supabase** cloud database and (optionally)
broadcast over **ESP-NOW** to a separate announcer ESP. The device keeps working
even with no internet, no router, and across reboots.

---

## 1. What it does

A typical shop runs **6 of these devices** (scanner + display per counter) plus an
optional **7th "gateway" ESP** that audibly announces scanned numbers.

Each scan drives three independent things:

1. **Display** — the token number is shown / cycled on the P10 panel.
2. **Supabase** — the scan is logged to the cloud (with offline queueing + retry).
3. **ESP-NOW** — the scan is mirrored to the gateway ESP (works even when the
   router is down).

These three paths are independent: losing the internet only stops #2 (and it
queues for later); losing the router doesn't stop #1 or #3.

### Scan lifecycle

The scanner reads a barcode. The expected format is `BY-YYYYMMDD-T<token>`, but a
relaxed mode (default) accepts any barcode and uses its **last 4 digits** as the
token.

| Order type | Detected by | 1st scan | 2nd scan |
|------------|-------------|----------|----------|
| **Offline / walk-in** | token is all digits | `ready_to_collect` → number **shown** on panel | `no_show` → number **removed**, ignored after |
| **Online** | token has letters (e.g. `ORD42`) | `packing_complete` (never displayed) | — |

Local state (which numbers are on the display, which are done) is persisted in
NVS so it survives reboots, and is wiped automatically every 6 hours.

---

## 2. Hardware

- **MCU:** ESP32-S3 (ESP32-WROOM-32 family), 4 MB flash.
- **Display:** single red **P10 32×16 HUB12 1/4-scan** LED matrix, bit-banged.
- **Input:** USB-HID **barcode scanner** (via the ESP32-S3 USB host).
- **Audio (gateway/optional):** DFPlayer Mini (component included).
- **Connectivity:** 2.4 GHz Wi-Fi (STA) for Supabase; ESP-NOW for gateway.

---

## 3. Project layout

```
smart_token_display/
├─ CMakeLists.txt          # top-level; derives version from `git describe`
├─ load_env.cmake          # reads .env → compile definitions (Supabase, gateway MAC)
├─ partitions.csv          # dual-OTA layout (ota_0 / ota_1, 4 MB)
├─ sdkconfig.defaults      # persistent build config (TLS bundle, log timestamps…)
├─ release.py              # build → Supabase Storage upload → publish OTA trigger
├─ .env / .env.example     # secrets + config (URL, keys, GATEWAY_MAC)
├─ db/
│  ├─ token_scans.sql      # token_events / token_status tables + record RPC
│  └─ rls_policies.sql     # RLS policies + device_status trigger fix
├─ main/
│  ├─ inc/                 # public headers (main, wifi, supabase, ota, espnow…)
│  └─ src/
│     ├─ main.c            # app_main: boot, SNTP/IST, starts everything
│     ├─ wifi.c            # Wi-Fi STA bring-up
│     ├─ provisioning.c    # SoftAP Wi-Fi provisioning + Wi-Fi event handler
│     ├─ supabase.c        # token-event RPC, heartbeat, realtime websocket
│     ├─ ota.c             # OTA update (toggle: ENABLE_OTA)
│     ├─ espnow.c          # ESP-NOW scan broadcast (toggle: ENABLE_ESPNOW)
│     └─ token_display.c   # scan parsing, lifecycle, display, offline queue
└─ components/
   ├─ barcode_scanner/     # USB-HID keyboard-wedge scanner driver
   ├─ p10display/          # P10 LED matrix driver
   └─ dfplayer/            # DFPlayer Mini audio driver
```

### Key source files

- **`token_display.c`** — the heart. Parses barcodes, runs the
  `ready_to_collect → no_show` lifecycle, cycles active tokens on the display,
  and owns the NVS-backed **offline event queue** (scans made offline are stored
  and flushed when connectivity returns).
- **`supabase.c`** — `post_token_event()` calls the `record_token_event` RPC;
  `send_device_health_to_supabase()` posts a heartbeat; a realtime websocket
  receives OTA + reprovision triggers.
- **`espnow.c`** — best-effort unicast of each scan to the gateway. Independent
  of the internet; survives router outages (see §7).
- **`ota.c`** — pulls a new firmware `.bin` when Supabase advertises a newer
  version over the realtime channel.

---

## 4. Feature toggles

Both major optional features compile out cleanly to no-op stubs when disabled.
Edit `main/inc/main.h`:

```c
#define ENABLE_OTA    1   // Over-the-air updates via Supabase realtime
#define ENABLE_ESPNOW 1   // Broadcast each scan to the gateway over ESP-NOW
```

---

## 5. Configuration (`.env`)

All secrets/config live in a single project-root `.env` (gitignored). The same
file is read by both the firmware build (`load_env.cmake`) and `release.py`.
Copy `.env.example` → `.env` and fill in:

```sh
SUPABASE_URL=https://<your-project>.supabase.co
SUPABASE_KEY=<anon-key>                       # baked into firmware
# SUPABASE_SERVICE_ROLE_KEY=<service-role>    # release.py uploads only (keep off-device)

# ESP-NOW gateway (announcer / 7th ESP) STA MAC. Each scan is unicast here.
# If unset, the firmware broadcasts to FF:FF:FF:FF:FF:FF instead.
GATEWAY_MAC=DE:AD:BE:EF:00:07
```

> The **anon key** (not the service-role key) is compiled into the device. The
> service-role key is used only by `release.py` for Storage uploads and must
> never be put on the device.

---

## 6. Database setup (Supabase)

Run these once in the Supabase SQL editor, in order:

1. **`db/token_scans.sql`** — creates `token_events`, `token_status`, and the
   `record_token_event(...)` RPC.
2. **`db/rls_policies.sql`** — makes the RPC `SECURITY DEFINER` (so the anon key
   can write without being blocked by Row-Level Security), adds `device_status`
   policies, and **fixes the `device_status` trigger** (it referenced a
   non-existent `updated_at` column; corrected to `last_seen`).

Without step 2 the device gets `HTTP 401 / "violates row-level security policy"`
and `device_status` rows silently fail to insert.

---

## 7. ESP-NOW + the single-channel rule (important for deployment)

The ESP32 radio is on **one channel at a time**. While connected to the router,
it sits on the **router's channel**, and ESP-NOW rides on that channel. For
ESP-NOW to keep working when the **router is off**, the whole fleet must share a
**fixed channel**:

1. **Lock the shop router's 2.4 GHz band to a fixed channel** (1, 6, or 11 — pick
   the least-crowded with a Wi-Fi analyzer app). This does **not** change the
   SSID/password; your phones/laptops auto-reconnect on the new channel.
2. Set **`ESPNOW_CHANNEL`** in `main/inc/espnow.h` to that same channel
   (default `1`). **Router channel and `ESPNOW_CHANNEL` must match.**
3. The gateway (7th ESP) is permanently pinned to that channel and never joins
   the router, so it's unaffected by outages.
4. On STA disconnect, senders re-pin the radio to `ESPNOW_CHANNEL`
   (`espnow_repin_channel()`), so announcements keep flowing with no router.

**Packet format** (`espnow_scan_msg_t` in `espnow.h`): a ~96-byte packed binary
struct (version, order_type, status, display_num, token, order_id, event_date,
sender MAC, seq) — well under the 250-byte ESP-NOW limit.

The gateway is a separate firmware: it sets `WIFI_PS_NONE`, pins to
`ESPNOW_CHANNEL`, `esp_now_init()`, registers a receive callback, checks
`version`, and announces `display_num`.

---

## 8. Build, flash, monitor

Requires **ESP-IDF v5.4.x**. From the project root:

```sh
. $IDF_PATH/export.sh          # set up the IDF environment

idf.py set-target esp32s3      # first time only
idf.py build                   # compile (version comes from `git describe`)
./flash_signed.sh -p /dev/ttyACM0   # sign + cable-flash (see §9a)
idf.py -p /dev/ttyACM0 monitor
```

> Use `./flash_signed.sh`, **not** `idf.py flash`, to cable-flash: this firmware
> requires a signed app, and `idf.py flash` writes the unsigned build (boot loop).
> See §9a.

The firmware version is derived from git tags, so tag releases cleanly:

```sh
git tag v1.2.0                 # then `idf.py build` embeds v1.2.0
```

### First boot — Wi-Fi provisioning
If the device has no saved Wi-Fi, it starts a **SoftAP provisioning** service
(`PROV_xxxxxx`). Use the ESP SoftAP Provisioning app (security 2, the QR link is
printed in the serial log) to send SSID/password. Credentials are stored in NVS;
later boots connect automatically. Scanning/display work **before** Wi-Fi is up.

---

## 9. OTA releases (`release.py`)

`release.py` builds nothing itself — it takes the already-built `build/*.bin`,
uploads it to Supabase Storage, and publishes a row that triggers connected
devices to update over the realtime websocket.

```sh
idf.py build                                   # build the new firmware first
git tag v1.2.0                                 # clean version (OTA compares exactly)
idf.py build                                   # rebuild so the .bin embeds v1.2.0

python3 release.py --version v1.2.0 --desc "What changed" --yes
```

What it does: stages `releases/<name>_v<ver>.bin`, **signs it** (see below),
uploads with retry + exponential backoff, then PATCHes/POSTs the `system_control`
row with the new version + public URL. Devices running an older version detect
the change via Realtime and OTA-update themselves.

> OTA compares the version **string exactly**. Always release from a clean git
> tag (no `-dirty` suffix) so the version is stable and you don't re-trigger OTA
> on every commit.

---

## 9a. Signed OTA (firmware authenticity)

The firmware **requires signed images**: the running app verifies the RSA-3072
signature of any new OTA image against its embedded public key before booting it
(`CONFIG_SECURE_SIGNED_*_NO_SECURE_BOOT`). A tampered or unsigned image is
refused — even over valid HTTPS from the correct host. This is layered on top of
the `bin_url` host check in `ota.c` (which rejects URLs not under your Supabase
Storage). No hardware Secure Boot / eFuse burn is used, so it is fully reversible.

**Key:** `secure_boot_signing_key.pem` (RSA-3072, project root). It is the **root
of trust** — gitignored, never commit it, and back it up off-machine. Lose it and
you can't push OTAs that signed devices accept (recovery = cable re-flash).

**The build is intentionally UNSIGNED** (`BUILD_SIGNED_BINARIES` off); `release.py`
signs at release time, so the key never lives in the firmware build config:

```sh
# release.py auto-signs the staged binary with the key, then uploads:
python3 release.py --version v1.2.0 --desc "..." --yes
#   key resolution: --sign-key <path>  >  SIGNING_KEY in .env  >  ./secure_boot_signing_key.pem
#   espsecure.py must be on PATH (run inside  . $IDF_PATH/export.sh)
```

> ⚠️ **First signed image must be cable-flashed.** A device can only verify OTA
> images once it is already running a signed app holding the public key. Flash the
> first signed build over USB on all devices (this coincides with the one-time
> partition-table cable flash, so it's a single visit). After that, signed OTAs
> work over the air. Never OTA/flash an `--no-sign` build to a signed device.

### Cable-flashing a signed build — `flash_signed.sh`

A plain `idf.py flash` writes the **unsigned** `build/<name>.bin`, which a signed
device rejects at boot (`secure_boot_v2: No signatures were found` → `abort()`
boot loop). Use the helper instead — it signs the built app with the project key
and flashes the **signed** image to the app partition:

```sh
. $IDF_PATH/export.sh        # espsecure.py / esptool.py on PATH
idf.py build
./flash_signed.sh            # auto-detect port; sign + flash app only
./flash_signed.sh -p /dev/ttyACM0
./flash_signed.sh --full     # also flash bootloader + partition table + otadata
./flash_signed.sh --no-stub  # if a boot loop floods the USB-serial port
```

It reads the project name, target chip, app offset and flash settings from
`build/` (nothing hardcoded), signs with `secure_boot_signing_key.pem`
(override via `--sign-key` / `SIGNING_KEY`), verifies the signature, then flashes.

Manual signing (for reference; `flash_signed.sh` and `release.py` do this for you):
```sh
espsecure.py sign_data --version 2 --keyfile secure_boot_signing_key.pem \
    --output build/signed.bin build/smart_token_display.bin
espsecure.py verify_signature --version 2 --keyfile secure_boot_signing_key.pem build/signed.bin
```

---

## 10. Logs & timestamps

Log lines use **system wall-clock time in IST** (UTC+5:30). The timezone is set
in `initialize_sntp()` (`TZ=IST-5:30`) and the timestamp source is configured in
`sdkconfig.defaults` (`CONFIG_LOG_TIMESTAMP_SOURCE_SYSTEM=y`). Before SNTP syncs,
timestamps count from the epoch; after sync they read correct local time.

---

## 11. Behavior summary

| Condition | Display | Supabase | ESP-NOW |
|-----------|---------|----------|---------|
| All up | ✅ | ✅ | ✅ |
| Internet down (router up) | ✅ | ⏳ queued in NVS, flushes later | ✅ |
| Router down | ✅ | ⏳ queued | ✅ (re-pinned to fixed channel) |
| Reboot | ✅ state restored from NVS | ✅ queue persists | ✅ |
```
