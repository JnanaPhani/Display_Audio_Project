#!/usr/bin/env python3
"""
Portable ESP-IDF -> Supabase OTA release tool (Audio Board version).

WHAT IT DOES
  1. Finds the compiled app .bin in build/ (via build/project_description.json,
     falling back to the newest *.bin in build/).
  2. Determines the version: --version arg > git describe > prompt.
  3. Copies it to releases/<name>_v<version>.bin.
  4. Uploads to Supabase Storage (upsert).
  5. PATCHes the system_control row to publish version + bin_url, triggering OTA.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

try:
    import requests
except ImportError:
    sys.exit("Missing dependency: pip install requests")


# --------------------------------------------------------------------------- #
# Config loading                                                              #
# --------------------------------------------------------------------------- #
def load_env_file(path=".env"):
    """Load simple KEY=VALUE lines into os.environ (does not overwrite real env)."""
    if not os.path.exists(path):
        return
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            key, val = key.strip(), val.strip().strip('"').strip("'")
            os.environ.setdefault(key, val)


def get_cfg(name, default=None, required=False):
    val = os.environ.get(name, default)
    if required and not val:
        sys.exit(
            f"Error: {name} is not set. Export it or add it to .env\n"
            f"  e.g.  export {name}=...   (see header of this script)"
        )
    return val


# --------------------------------------------------------------------------- #
# Auto-detection helpers                                                       #
# --------------------------------------------------------------------------- #
def find_build_dir():
    for d in ("build", os.environ.get("BUILD_DIR", "")):
        if d and os.path.isdir(d):
            return d
    return "build"


def detect_project_name(build_dir):
    """Read the IDF project name from build/project_description.json if present."""
    pdj = os.path.join(build_dir, "project_description.json")
    if os.path.exists(pdj):
        try:
            with open(pdj) as f:
                return json.load(f).get("project_name")
        except (json.JSONDecodeError, OSError):
            pass
    return None


def detect_bin(build_dir):
    """Locate the app .bin without hardcoding the project name."""
    # 1. Explicit override
    override = os.environ.get("RELEASE_BIN")
    if override:
        return override

    # 2. From project_description.json (authoritative)
    name = detect_project_name(build_dir)
    if name:
        candidate = os.path.join(build_dir, f"{name}.bin")
        if os.path.exists(candidate):
            return candidate

    # 3. Fallback: newest .bin in build/, excluding known non-app artifacts
    skip = ("bootloader.bin", "partition-table.bin", "ota_data_initial.bin")
    bins = [
        p for p in glob.glob(os.path.join(build_dir, "*.bin"))
        if os.path.basename(p) not in skip
    ]
    if not bins:
        return None
    return max(bins, key=os.path.getmtime)


def detect_version_from_project_desc(build_dir):
    pdj = os.path.join(build_dir, "project_description.json")
    if os.path.exists(pdj):
        try:
            with open(pdj) as f:
                v = json.load(f).get("project_version")
                if v:
                    return v
        except (json.JSONDecodeError, OSError):
            pass
    return None


def detect_version_from_git():
    try:
        out = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            capture_output=True, text=True, timeout=10,
        )
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except (FileNotFoundError, subprocess.SubprocessError):
        pass
    return None


def sanitize(version):
    """Make a filesystem/URL-safe token from a version string."""
    return "".join(c if c.isalnum() else "_" for c in version)


# --------------------------------------------------------------------------- #
# Firmware signing (RSA-3072, Secure Boot v2 app-signing scheme)              #
# --------------------------------------------------------------------------- #
def resolve_signing_key(cli_key):
    """Locate the signing key: --sign-key > SIGNING_KEY env > project default."""
    candidate = cli_key or os.environ.get("SIGNING_KEY") or "secure_boot_signing_key.pem"
    return candidate


def sign_image(bin_path, key_path):
    """Sign a firmware .bin IN PLACE with espsecure.py (Secure Boot v2 scheme)."""
    if not os.path.exists(key_path):
        sys.exit(
            f"Error: signing key not found at '{key_path}'.\n"
            f"  The firmware requires signed OTA images. Provide the key via\n"
            f"  --sign-key <path>, SIGNING_KEY=... in .env, or place it at\n"
            f"  ./secure_boot_signing_key.pem"
        )
    if shutil.which("espsecure.py") is None:
        sys.exit(
            "Error: espsecure.py not found on PATH. Run this inside the ESP-IDF\n"
            "  environment:  . $IDF_PATH/export.sh"
        )

    # Sign to a temp file, then replace the original so the uploaded + staged
    # binary is the signed one.
    signed_tmp = bin_path + ".signed"
    cmd = [
        "espsecure.py", "sign_data", "--version", "2",
        "--keyfile", key_path, "--output", signed_tmp, bin_path,
    ]
    print(f"Signing : {bin_path}  (key: {key_path})")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"Signing failed:\n{result.stdout}\n{result.stderr}")
    os.replace(signed_tmp, bin_path)
    print("Signed  : OK (RSA-3072 signature block appended)")


# --------------------------------------------------------------------------- #
# Supabase actions                                                             #
# --------------------------------------------------------------------------- #
def upload_to_storage(base_url, key, bucket, filename, data, max_attempts=6):
    url = f"{base_url}/storage/v1/object/{bucket}/{filename}"
    headers = {
        "apikey": key,
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/octet-stream",
        "x-upsert": "true",
    }
    for attempt in range(max_attempts):
        try:
            print(f"  Upload attempt {attempt + 1}/{max_attempts} ({len(data)} bytes)...")
            r = requests.post(url, headers=headers, data=data, timeout=(30, 300))
            if r.status_code == 200:
                print("  Upload successful.")
                return True
            print(f"  Server returned {r.status_code}: {r.text[:200]}")
            if 400 <= r.status_code < 500 and r.status_code != 429:
                return False
        except (requests.exceptions.ConnectionError,
                requests.exceptions.Timeout,
                requests.exceptions.SSLError) as e:
            print(f"  Network error: {type(e).__name__} (transient on high-latency links)")
        if attempt < max_attempts - 1:
            backoff = min(30, 2 ** attempt)
            print(f"  Retrying in {backoff}s...")
            time.sleep(backoff)
    return False


def publish_row(base_url, key, table, row_id, payload):
    body = dict(payload)
    body["id"] = int(row_id) if str(row_id).isdigit() else row_id
    url = f"{base_url}/rest/v1/{table}?on_conflict=id"
    headers = {
        "apikey": key,
        "Authorization": f"Bearer {key}",
        "Content-Type": "application/json",
        "Prefer": "resolution=merge-duplicates,return=minimal",
    }
    r = requests.post(url, headers=headers, json=body, timeout=30)
    return r


# --------------------------------------------------------------------------- #
# Main                                                                         #
# --------------------------------------------------------------------------- #
def main():
    parser = argparse.ArgumentParser(description="ESP-IDF -> Supabase OTA release tool (Audio Board)")
    parser.add_argument("--version", help="Override version (default: git describe)")
    parser.add_argument("--desc", help="Update description")
    parser.add_argument("--bin", help="Path to firmware .bin (default: auto-detect)")
    parser.add_argument("--yes", action="store_true", help="Non-interactive; no prompts")
    parser.add_argument("--canary", metavar="MAC",
                        help="Target ONLY this device MAC (AA:BB:..).")
    parser.add_argument("--promote", action="store_true",
                        help="Clear target_device_id (NULL) so the whole fleet updates.")
    parser.add_argument("--sign-key", metavar="PEM",
                        help="Path to the RSA-3072 signing key.")
    parser.add_argument("--no-sign", action="store_true",
                        help="Skip signing. For debugging only.")
    parser.add_argument("--device-type", default="speaker",
                        help="Target device type (defaults to 'speaker').")
    args = parser.parse_args()

    load_env_file()
    base_url = get_cfg("SUPABASE_URL", required=True).rstrip("/")
    key = (get_cfg("SUPABASE_SERVICE_ROLE_KEY")
           or get_cfg("SUPABASE_SERVICE_KEY")
           or get_cfg("SUPABASE_KEY", required=True))
    bucket = get_cfg("STORAGE_BUCKET", "firmware")
    table = get_cfg("TABLE_NAME", "system_control")
    row_id = get_cfg("TABLE_ROW_ID", "1")

    build_dir = find_build_dir()
    print("--- ESP-IDF Supabase Auto-Release Tool (Audio Board) ---")

    if args.promote:
        r = publish_row(base_url, key, table, row_id, {"target_device_id": None})
        if r.status_code in (200, 201, 204):
            print("PROMOTED: target_device_id cleared -> whole fleet will update.")
            return
        sys.exit(f"Promote failed! {r.status_code}: {r.text}")

    # Binary
    src_bin = args.bin or detect_bin(build_dir)
    if not src_bin or not os.path.exists(src_bin):
        sys.exit(f"Error: firmware .bin not found (looked in '{build_dir}/').")
    project_name = detect_project_name(build_dir) or \
        os.path.splitext(os.path.basename(src_bin))[0]
    print(f"Project : {project_name}")
    print(f"Binary  : {src_bin}")

    # Device Type Resolution
    device_type = args.device_type
    print(f"Device Type: {device_type}")

    # Version
    version = (args.version
               or detect_version_from_project_desc(build_dir)
               or detect_version_from_git())
    if not version and not args.yes:
        version = input("Enter version (e.g. 1.0.5): ").strip()
    if not version:
        sys.exit("Error: could not determine version.")
    if version.endswith("-dirty") or "-dirty" in version:
        print(f"WARNING: version '{version}' is dirty (uncommitted changes).")
        if not args.yes and input("Continue anyway? [y/N]: ").strip().lower() != "y":
            sys.exit("Aborted.")
    print(f"Version : {version}")

    # Description
    desc = args.desc
    if desc is None:
        desc = "" if args.yes else input("Update description: ").strip()

    # Local release copy
    os.makedirs("releases", exist_ok=True)
    new_filename = f"{project_name}_v{sanitize(version)}.bin"
    release_path = os.path.join("releases", new_filename)
    shutil.copy(src_bin, release_path)
    print(f"Staged  : {release_path}")

    # Sign the staged binary
    if args.no_sign:
        print("WARNING: --no-sign set; uploading UNSIGNED image.")
    else:
        sign_image(release_path, resolve_signing_key(args.sign_key))

    # Keep the .elf
    elf_src = os.path.join(build_dir, f"{project_name}.elf")
    if os.path.exists(elf_src):
        elf_dst = os.path.join("releases", f"{project_name}_v{sanitize(version)}.elf")
        shutil.copy(elf_src, elf_dst)
        print(f"Staged  : {elf_dst}  (for crash decoding)")
    else:
        print("Note: .elf not found; crash backtraces for this build can't be decoded.")

    # Upload
    print(f"Uploading to bucket '{bucket}'...")
    with open(release_path, "rb") as f:
        data = f.read()
    if not upload_to_storage(base_url, key, bucket, new_filename, data):
        sys.exit("Upload failed after retries.")

    public_url = f"{base_url}/storage/v1/object/public/{bucket}/{new_filename}"

    # Publish
    print(f"Publishing to table '{table}' (id={row_id})...")
    payload = {"version": version, "bin_url": public_url, "device_type": device_type}
    if desc:
        payload["update_description"] = desc
    payload["target_device_id"] = args.canary if args.canary else None
    r = publish_row(base_url, key, table, row_id, payload)
    if r.status_code in (200, 201, 204):
        print("--------------------------------------------------")
        if args.canary:
            print(f"CANARY RELEASE {version} -> {args.canary} ONLY.")
            print("Watch ota_status for phase='success' from that device, then run:")
            print(f"  python release.py --promote")
            print("to roll out to the whole fleet.")
        else:
            print(f"RELEASE {version} IS LIVE (whole fleet)!")
        print(f"URL: {public_url}")
        print("Devices will detect the update via Realtime.")
        print("--------------------------------------------------")
    else:
        sys.exit(f"DB publish failed! {r.status_code}: {r.text}")


if __name__ == "__main__":
    main()
