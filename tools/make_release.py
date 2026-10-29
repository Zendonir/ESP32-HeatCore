#!/usr/bin/env python3
"""Packages a PlatformIO build as a HeatCore release.

    python3 tools/make_release.py --build .pio/build/kc868_a16v3 --out dist \
        [--version v0.5.0] [--repo owner/name] [--notes notes.txt]

Output (all in --out):
  heatcore-ota.bin       application image for the OTA update in the web UI
  heatcore-factory.bin   complete image (bootloader, partitions, app) for USB
  release.json           version, size and SHA-256 (read by the web UI)
  manifest.json          ESP Web Tools manifest (web flasher on GitHub Pages)
  SHA256SUMS, NOTES.md

The images are checked with the same rules the controller applies to an
upload (ESP32-S3 application image, HeatCore marker of the board) so that a
release can never be rejected by the device.
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import sys

BOARD = "KC868-A16V3"
CHIP_ESP32S3 = 0x0009
APP_DESC_MAGIC = 0xABCD5432
APP_OFFSET = 0x10000  # app0 in partitions/heatcore_16MB.csv


def fail(msg):
    sys.exit("make_release: " + msg)


def check_app_image(data, what):
    if len(data) < 48 or data[0] != 0xE9:
        fail(what + ": kein ESP32-Image")
    chip = struct.unpack_from("<H", data, 12)[0]
    if chip != CHIP_ESP32S3:
        fail(what + ": Chip-ID 0x%04x statt ESP32-S3" % chip)
    if struct.unpack_from("<I", data, 32)[0] != APP_DESC_MAGIC:
        fail(what + ": kein Anwendungs-Image")
    m = re.search(rb"HCFW1\|" + re.escape(BOARD.encode()) + rb"\|([0-9A-Za-z.+_-]{1,31})\x00", data)
    if not m:
        fail(what + ": HeatCore-Kennung fuer " + BOARD + " fehlt")
    return m.group(1).decode()


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default="")
    ap.add_argument("--repo", default="")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()

    with open(os.path.join(a.build, "firmware.bin"), "rb") as f:
        app = f.read()
    with open(os.path.join(a.build, "firmware.factory.bin"), "rb") as f:
        factory = f.read()

    version = check_app_image(app, "firmware.bin")
    want = a.version[1:] if a.version[:1] in ("v", "V") else a.version
    if want and want != version:
        fail("Version im Image (%s) passt nicht zum Release (%s)" % (version, want))
    # the factory image must carry the same application at the app0 offset
    if factory[:1] != b"\xe9":
        fail("firmware.factory.bin beginnt nicht mit dem Bootloader")
    if factory[APP_OFFSET:APP_OFFSET + len(app)] != app:
        fail("firmware.factory.bin enthaelt firmware.bin nicht an 0x%x" % APP_OFFSET)

    os.makedirs(a.out, exist_ok=True)
    files = {"heatcore-ota.bin": app, "heatcore-factory.bin": factory}
    for name, data in files.items():
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(data)

    notes = ""
    if a.notes and os.path.exists(a.notes):
        with open(a.notes, encoding="utf-8") as f:
            notes = f.read().strip()
    tag = "v" + version
    release_url = "https://github.com/%s/releases/tag/%s" % (a.repo, tag) if a.repo else ""
    today = datetime.date.today().isoformat()

    info = {
        "version": version,
        "board": BOARD,
        "date": today,
        "notes": notes,
        "releaseUrl": release_url,
        "ota": {"file": "heatcore-ota.bin", "size": len(app), "sha256": sha256(app)},
        "factory": {"file": "heatcore-factory.bin", "size": len(factory), "sha256": sha256(factory)},
    }
    with open(os.path.join(a.out, "release.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=1, ensure_ascii=False)

    manifest = {
        "name": "HeatCore " + BOARD,
        "version": version,
        "new_install_prompt_erase": True,
        "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "firmware/heatcore-factory.bin", "offset": 0}]}],
    }
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)

    with open(os.path.join(a.out, "SHA256SUMS"), "w") as f:
        for name, data in files.items():
            f.write("%s  %s\n" % (sha256(data), name))

    with open(os.path.join(a.out, "NOTES.md"), "w", encoding="utf-8") as f:
        f.write("HeatCore %s fuer %s\n\n" % (version, BOARD))
        if notes:
            f.write("## Aenderungen\n\n%s\n\n" % notes)
        f.write("## Dateien\n\n")
        f.write("| Datei | Zweck | SHA-256 |\n|---|---|---|\n")
        f.write("| `heatcore-ota.bin` | Update im Webinterface (System → Firmware-Update) | `%s` |\n" % sha256(app))
        f.write("| `heatcore-factory.bin` | Erstinstallation per USB (Web-Flasher oder esptool, Adresse 0x0) | `%s` |\n"
                % sha256(factory))
        f.write("\nFlashen ohne Installation: Web-Flasher auf der GitHub-Pages-Seite des Projekts "
                "(Chrome/Edge, USB-C). Details: docs/build-und-flash.md\n")

    print("HeatCore %s: ota %d bytes, factory %d bytes -> %s" % (version, len(app), len(factory), a.out))


if __name__ == "__main__":
    main()
