#!/usr/bin/env bash
# flash_signed.sh — sign the built app and cable-flash it to a device.
#
# The firmware enforces signed-OTA verification (CONFIG_SECURE_SIGNED_*), but the
# build intentionally produces an UNSIGNED build/<name>.bin (release.py signs at
# release time). A plain `idf.py flash` therefore writes an unsigned app, which
# the device rejects at boot:
#
#     E secure_boot_v2: No signatures were found for the running app
#     abort() ... check_signature_on_update_check  -> permanent boot loop
#
# This script closes that gap for the FIRST cable flash (and any later one): it
# signs build/<name>.bin with the same RSA-3072 key release.py uses, then flashes
# the SIGNED image to the app's flash offset.
#
# Usage:
#   . $IDF_PATH/export.sh              # espsecure.py / esptool.py must be on PATH
#   ./flash_signed.sh                  # auto-detect port, sign + flash app only
#   ./flash_signed.sh -p /dev/ttyACM0  # explicit port
#   ./flash_signed.sh --full           # also flash bootloader + partition table + otadata
#
# Env / flags:
#   PORT or -p <port>     serial port (default: first /dev/ttyACM*|ttyUSB*)
#   CHIP or --chip <c>    target chip (default: read from build, else esp32s3)
#   SIGNING_KEY or
#     --sign-key <pem>    signing key (default: ./secure_boot_signing_key.pem)
#   --no-stub             flash without the stub (needed when a boot loop floods
#                         a native USB-serial port and corrupts the handshake)
#   --full                flash all images, not just the app
set -euo pipefail

cd "$(dirname "$0")"

PORT="${PORT:-}"
CHIP="${CHIP:-}"
KEY="${SIGNING_KEY:-secure_boot_signing_key.pem}"
NO_STUB=""
FULL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -p|--port)      PORT="$2"; shift 2 ;;
        --chip)         CHIP="$2"; shift 2 ;;
        --sign-key)     KEY="$2"; shift 2 ;;
        --no-stub)      NO_STUB="--no-stub"; shift ;;
        --full)         FULL=1; shift ;;
        -h|--help)      grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)              echo "Unknown arg: $1" >&2; exit 2 ;;
    esac
done

BUILD_DIR="build"
DESC_JSON="$BUILD_DIR/project_description.json"
FLASHER_JSON="$BUILD_DIR/flasher_args.json"

# --- Preconditions ---------------------------------------------------------- #
command -v espsecure.py >/dev/null 2>&1 || command -v espsecure >/dev/null 2>&1 || {
    echo "Error: espsecure not on PATH. Run:  . \$IDF_PATH/export.sh" >&2; exit 1; }
command -v esptool.py   >/dev/null 2>&1 || command -v esptool   >/dev/null 2>&1 || {
    echo "Error: esptool not on PATH. Run:  . \$IDF_PATH/export.sh" >&2; exit 1; }
ESPSECURE="$(command -v espsecure.py || command -v espsecure)"
ESPTOOL="$(command -v esptool.py || command -v esptool)"

[[ -f "$FLASHER_JSON" ]] || { echo "Error: $FLASHER_JSON missing. Run 'idf.py build' first." >&2; exit 1; }
[[ -f "$KEY" ]] || { echo "Error: signing key not found: $KEY" >&2; exit 1; }

# --- Resolve project name, chip, app offset from the build ------------------ #
read_json() { python3 -c "import json,sys;print(json.load(open(sys.argv[1])).get(sys.argv[2],'') or '')" "$1" "$2"; }

PROJECT_NAME="$(read_json "$DESC_JSON" project_name)"
[[ -n "$PROJECT_NAME" ]] || PROJECT_NAME="smart_token_display"
APP_BIN="$BUILD_DIR/${PROJECT_NAME}.bin"
[[ -f "$APP_BIN" ]] || { echo "Error: app binary not found: $APP_BIN. Run 'idf.py build'." >&2; exit 1; }

if [[ -z "$CHIP" ]]; then
    CHIP="$(read_json "$DESC_JSON" target)"
    [[ -n "$CHIP" ]] || CHIP="esp32s3"
fi

# App flash offset = the key in flasher_args.json flash_files whose value is the
# app .bin (so we never hardcode 0x30000).
APP_OFFSET="$(python3 - "$FLASHER_JSON" "${PROJECT_NAME}.bin" <<'PY'
import json,sys
d=json.load(open(sys.argv[1])); needle=sys.argv[2]
for off,f in d.get("flash_files",{}).items():
    if f.endswith(needle):
        print(off); break
PY
)"
[[ -n "$APP_OFFSET" ]] || { echo "Error: could not find app offset in $FLASHER_JSON" >&2; exit 1; }

FLASH_MODE="$(python3 -c "import json;print(json.load(open('$FLASHER_JSON'))['flash_settings'].get('flash_mode','dio'))")"
FLASH_FREQ="$(python3 -c "import json;print(json.load(open('$FLASHER_JSON'))['flash_settings'].get('flash_freq','80m'))")"

# --- Auto-detect port ------------------------------------------------------- #
if [[ -z "$PORT" ]]; then
    for p in /dev/ttyACM* /dev/ttyUSB*; do
        [[ -e "$p" ]] && { PORT="$p"; break; }
    done
    [[ -n "$PORT" ]] || { echo "Error: no serial port found. Pass -p <port>." >&2; exit 1; }
fi

# --- Sign ------------------------------------------------------------------- #
SIGNED_BIN="$BUILD_DIR/${PROJECT_NAME}-signed.bin"
echo "Signing : $APP_BIN  (key: $KEY)"
"$ESPSECURE" sign_data --version 2 --keyfile "$KEY" --output "$SIGNED_BIN" "$APP_BIN"
"$ESPSECURE" verify_signature --version 2 --keyfile "$KEY" "$SIGNED_BIN" >/dev/null
echo "Signed  : $SIGNED_BIN (verified)"

# --- Flash ------------------------------------------------------------------ #
COMMON=( --chip "$CHIP" --port "$PORT" $NO_STUB --before default_reset --after hard_reset
         write_flash --flash_mode "$FLASH_MODE" --flash_freq "$FLASH_FREQ" )

if [[ "$FULL" -eq 1 ]]; then
    # Flash everything, but substitute the SIGNED app for the unsigned one.
    ARGS=()
    while read -r off f; do
        [[ -z "$off" ]] && continue
        if [[ "$f" == *"${PROJECT_NAME}.bin" ]]; then
            ARGS+=( "$off" "$SIGNED_BIN" )
        else
            ARGS+=( "$off" "$BUILD_DIR/$f" )
        fi
    done < <(python3 -c "import json;[print(o,f) for o,f in json.load(open('$FLASHER_JSON'))['flash_files'].items()]")
    echo "Flashing: full image set to $PORT (app signed)"
    "$ESPTOOL" "${COMMON[@]}" "${ARGS[@]}"
else
    echo "Flashing: signed app -> $APP_OFFSET on $PORT"
    "$ESPTOOL" "${COMMON[@]}" "$APP_OFFSET" "$SIGNED_BIN"
fi

echo
echo "Done. The device should boot the signed app (no secure_boot abort loop)."
echo "Tip: if flashing fails with 'Invalid head of packet', the device is in a"
echo "     boot loop flooding the port — re-run with --no-stub."
