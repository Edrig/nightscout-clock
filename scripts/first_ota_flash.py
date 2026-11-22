#!/usr/bin/env python3
"""
One-time USB flash to the OTA build, keeping the clock's settings.

The OTA build uses a different partition table (two app slots and a smaller LittleFS), so the first flash has to
go over USB. This script:
  1. reads the clock's current partition table and LittleFS over USB and saves its config.json
     (WiFi, Nightscout, faces, ...) next to the script as a backup,
  2. downloads the latest OTA release from GitHub (partition table, firmware, LittleFS image) and checks
     the SHA-256 digests GitHub publishes,
  3. puts the saved config.json into the new LittleFS image,
  4. writes everything to the clock and restarts it.
The NVS partition is not touched. Later updates go over WiFi from the settings page.

Needs:   pip install esptool littlefs-python
Usage:   python scripts/first_ota_flash.py --port COM5        (Windows)
         python scripts/first_ota_flash.py --port /dev/ttyUSB0 (Linux / macOS: /dev/cu.usbserial-...)
Options: --repo owner/name   GitHub repository of the OTA releases (default below)
         --config file.json  use this settings file instead of reading it from the clock
         --dry-run           read and prepare everything, write nothing to the clock
"""

import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

DEFAULT_REPO = "Edrig/nightscout-clock"
BLOCK_SIZE = 4096
DISK_VERSION_2_0 = 0x00020000
CONFIG_PATH = "/config.json"
OTADATA_OFFSET = 0xE000
PARTITION_TABLE_OFFSET = 0x8000
PARTITION_TABLE_SIZE = 0xC00
NEEDED_ASSETS = ("partitions.bin", "boot_app0.bin", "firmware.bin", "littlefs.bin")

try:
    from littlefs import LittleFS
except ImportError:
    sys.exit("littlefs-python is missing: pip install esptool littlefs-python")


def say(text):
    print(f"\n==> {text}", flush=True)


def esptool(port, *args):
    """Run esptool from the current Python, stop on failure."""
    command = [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", port, *args]
    result = subprocess.run(command)
    if result.returncode != 0:
        sys.exit(f"esptool failed: {' '.join(command)}")


def parse_partitions(table):
    """Return [(name, type, subtype, offset, size)] from a binary ESP32 partition table."""
    parts = []
    for i in range(0, len(table), 32):
        entry = table[i:i + 32]
        if len(entry) < 32:
            break
        magic, ptype, subtype, offset, size, name, _flags = struct.unpack("<HBBII16sI", entry)
        if magic != 0x50AA:
            break
        parts.append((name.rstrip(b"\0").decode(errors="replace"), ptype, subtype, offset, size))
    return parts


def find_littlefs(parts):
    for name, ptype, subtype, offset, size in parts:
        # data / spiffs (0x82) or data / littlefs (0x83)
        if ptype == 0x01 and subtype in (0x82, 0x83):
            return name, offset, size
    return None


def mount(image):
    # Write on-disk version 2.0: the clock's littlefs may be older than 2.6 and unable to mount 2.1.
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=len(image) // BLOCK_SIZE, disk_version=DISK_VERSION_2_0,
                  mount=False)
    fs.context.buffer = bytearray(image)
    fs.mount()
    return fs


def read_config_from_clock(port, workdir):
    say("Reading the clock's partition table")
    table_file = os.path.join(workdir, "old_partitions.bin")
    esptool(port, "read_flash", hex(PARTITION_TABLE_OFFSET), hex(PARTITION_TABLE_SIZE), table_file)
    parts = parse_partitions(open(table_file, "rb").read())
    for name, ptype, subtype, offset, size in parts:
        print(f"    {name:10s} type {ptype:#04x} subtype {subtype:#04x} at {offset:#08x}, {size // 1024} KB")
    fs_part = find_littlefs(parts)
    if not fs_part:
        sys.exit("No LittleFS partition found on the clock. Use --config with a settings file downloaded from the "
                 "clock's settings page.")

    name, offset, size = fs_part
    say(f"Reading the clock's LittleFS ({size // 1024} KB at {offset:#x}), this takes a little while")
    fs_file = os.path.join(workdir, "old_littlefs.bin")
    esptool(port, "read_flash", hex(offset), hex(size), fs_file)
    try:
        fs = mount(open(fs_file, "rb").read())
        with fs.open(CONFIG_PATH, "r") as f:
            text = f.read()
    except Exception as e:  # noqa: BLE001 - any littlefs error means the same thing here
        sys.exit(f"Could not read {CONFIG_PATH} from the clock ({e}). Download the settings from the clock's "
                 "settings page and run again with --config <file>.")
    return text


def fetch_release(repo, workdir):
    say(f"Downloading the latest release of {repo}")
    request = urllib.request.Request(f"https://api.github.com/repos/{repo}/releases/latest",
                                     headers={"Accept": "application/vnd.github+json",
                                              "User-Agent": "nightscout-clock-first-flash"})
    with urllib.request.urlopen(request, timeout=30) as response:
        release = json.load(response)
    assets = {a["name"]: a for a in release.get("assets", [])}
    missing = [n for n in NEEDED_ASSETS if n not in assets]
    if missing:
        sys.exit(f"Release {release.get('tag_name')} has no {', '.join(missing)}. Publish a new release first.")

    files = {}
    for name in NEEDED_ASSETS:
        asset = assets[name]
        request = urllib.request.Request(asset["browser_download_url"],
                                         headers={"User-Agent": "nightscout-clock-first-flash"})
        with urllib.request.urlopen(request, timeout=120) as response:
            data = response.read()
        digest = asset.get("digest") or ""
        if digest.startswith("sha256:") and hashlib.sha256(data).hexdigest() != digest[7:].lower():
            sys.exit(f"{name}: checksum mismatch, stopping.")
        path = os.path.join(workdir, name)
        open(path, "wb").write(data)
        files[name] = path
        print(f"    {name}: {len(data)} bytes{' (checksum ok)' if digest else ''}")
    return release.get("tag_name", "?"), files


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True, help="Serial port of the clock, for example COM5 or /dev/ttyUSB0")
    parser.add_argument("--repo", default=DEFAULT_REPO, help=f"GitHub repository (default {DEFAULT_REPO})")
    parser.add_argument("--config", help="Settings file to install instead of reading it from the clock")
    parser.add_argument("--dry-run", action="store_true", help="Prepare everything but write nothing")
    args = parser.parse_args()

    workdir = tempfile.mkdtemp(prefix="nsclock-flash-")

    # 1. Settings
    if args.config:
        config_text = open(args.config, "r", encoding="utf-8").read()
    else:
        config_text = read_config_from_clock(args.port, workdir)
    try:
        config = json.loads(config_text)
    except json.JSONDecodeError as e:
        sys.exit(f"The settings are not valid JSON ({e}), stopping.")
    backup = os.path.join(os.getcwd(), f"clock-settings-backup-{time.strftime('%Y%m%d-%H%M%S')}.json")
    open(backup, "w", encoding="utf-8").write(config_text)
    print(f"    Settings saved to {backup} (WiFi: {config.get('ssid') or 'not set'})")

    # 2. Release
    tag, files = fetch_release(args.repo, workdir)

    new_parts = parse_partitions(open(files["partitions.bin"], "rb").read())
    fs_part = find_littlefs(new_parts)
    app_part = next((p for p in new_parts if p[1] == 0x00 and p[2] == 0x10), None)  # app / ota_0
    if not fs_part or not app_part:
        sys.exit("The release's partition table has no ota_0 app or no LittleFS partition, stopping.")
    _, fs_offset, fs_size = fs_part
    app_offset = app_part[3]
    if os.path.getsize(files["littlefs.bin"]) != fs_size:
        sys.exit("littlefs.bin does not match the size of the release's LittleFS partition, stopping.")

    # 3. Settings into the new filesystem image
    say("Putting the settings into the new LittleFS image")
    fs = mount(open(files["littlefs.bin"], "rb").read())
    with fs.open(CONFIG_PATH, "w") as f:
        f.write(config_text)
    with fs.open(CONFIG_PATH, "r") as f:
        if f.read() != config_text:
            sys.exit("The settings could not be written into the image, stopping.")
    image = os.path.join(workdir, "littlefs_with_settings.bin")
    open(image, "wb").write(bytes(fs.context.buffer))
    print(f"    {', '.join(fs.listdir('/'))}")

    if args.dry_run:
        say(f"Dry run: nothing written. Files are in {workdir}")
        return

    # 4. Flash
    say(f"Writing release {tag} to the clock (partition table, firmware, LittleFS). Do not unplug it.")
    esptool(args.port, "--baud", "921600", "write_flash",
            hex(PARTITION_TABLE_OFFSET), files["partitions.bin"],
            hex(OTADATA_OFFSET), files["boot_app0.bin"],
            hex(app_offset), files["firmware.bin"],
            hex(fs_offset), image)
    say("Done. The clock restarts with its settings; next updates go over WiFi "
        "(settings page > WiFi & system > Update over WiFi).")


if __name__ == "__main__":
    main()
