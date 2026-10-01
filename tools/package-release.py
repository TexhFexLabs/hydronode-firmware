#!/usr/bin/env python3
"""Packages built firmware into a release folder the backend can serve.

For every chip family: bootloader + partition table + boot_app0 + app are merged
into one image that is flashed at offset 0. The web flasher then writes the
config block at the "hncfg" offset. Output:

  dist/release/<version>/
    manifest.json            images, hashes, config offset
    <family>.bin             merged image per chip family
    catalog.json             boards, drivers, sleep modes, libraries
    THIRD_PARTY_LICENSES.md  from tools/license-check.mjs
    SHA256SUMS
  dist/hydronode-firmware-<version>.zip

Run after `pio run`, `node tools/validate-catalog.mjs --out dist` and
`node tools/license-check.mjs --out dist/THIRD_PARTY_LICENSES.md`.
"""

import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PIO_HOME = Path(os.environ.get("PLATFORMIO_CORE_DIR", Path.home() / ".platformio"))
# esptool comes from pip (pinned in release.yml), not from ~/.platformio: the ESP8266 platform
# installs an old tool-esptoolpy under the same name, which has no merge-bin.
ESPTOOL_MAJOR = "5."
BOOT_APP0 = PIO_HOME / "packages" / "framework-arduinoespressif32" / "tools" / "partitions" / "boot_app0.bin"

PARTITION_TABLE_OFFSET = 0x8000
CHIP_ARG = {"esp32": "esp32", "esp32s2": "esp32s2", "esp32s3": "esp32s3", "esp32c3": "esp32c3", "esp32c6": "esp32c6", "esp8266": "esp8266"}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def partitions() -> dict:
    rows = {}
    with open(ROOT / "partitions" / "hydronode_4mb.csv") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            name, _type, _sub, offset, size = [c.strip() for c in row[:5]]
            rows[name] = {"offset": int(offset, 0), "size": int(size, 0)}
    return rows


def main() -> int:
    lines = subprocess.run([sys.executable, "-m", "esptool", "version"], capture_output=True, text=True).stdout.split()
    found = lines[-1].lstrip("v") if lines else "none"
    if not found.startswith(ESPTOOL_MAJOR):
        sys.exit(f"esptool {ESPTOOL_MAJOR}x needed for merge-bin, found {found} (pip install esptool==5.4.0)")
    ini = (ROOT / "platformio.ini").read_text()
    version = re.search(r"^version\s*=\s*(\S+)", ini, re.M).group(1)
    catalog = json.loads((ROOT / "dist" / "catalog.json").read_text())
    if catalog["firmwareVersion"] != version:
        sys.exit(f"dist/catalog.json is for {catalog['firmwareVersion']}, expected {version}")
    licenses = ROOT / "dist" / "THIRD_PARTY_LICENSES.md"
    if not licenses.exists():
        sys.exit("dist/THIRD_PARTY_LICENSES.md missing, run tools/license-check.mjs first")

    parts = partitions()
    out = ROOT / "dist" / "release" / version
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)

    images = {}
    for family, spec in catalog["families"].items():
        build = ROOT / ".pio" / "build" / spec["env"]
        target = out / f"{family}.bin"
        if family == "esp8266":
            # No partition table: firmware.bin already starts with the eboot loader, flashed at 0.
            app = build / "firmware.bin"
            if not app.exists():
                sys.exit(f"{family}: missing {app}, run pio run -e {spec['env']}")
            shutil.copy(app, target)
            if target.stat().st_size > spec["configOffset"]:
                sys.exit(f"{family}: image overlaps the config area")
            images[family] = {
                "file": target.name,
                "chip": spec["chip"],
                "offset": 0,
                "size": target.stat().st_size,
                "sha256": sha256(target),
                "configOffset": spec["configOffset"],
            }
            continue
        needed = [build / "bootloader.bin", build / "partitions.bin", build / "firmware.bin"]
        missing = [str(p) for p in needed if not p.exists()]
        if missing:
            sys.exit(f"{family}: missing {missing}, run pio run -e {spec['env']}")
        subprocess.run(
            [
                sys.executable, "-m", "esptool", "--chip", CHIP_ARG[family], "merge-bin",
                "-o", str(target), "--flash-mode", "dio", "--flash-size", "4MB",
                hex(spec["bootloaderOffset"]), str(build / "bootloader.bin"),
                hex(PARTITION_TABLE_OFFSET), str(build / "partitions.bin"),
                hex(parts["otadata"]["offset"]), str(BOOT_APP0),
                hex(parts["app0"]["offset"]), str(build / "firmware.bin"),
            ],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        if target.stat().st_size > parts["hncfg"]["offset"]:
            sys.exit(f"{family}: merged image overlaps the config partition")
        images[family] = {
            "file": target.name,
            "chip": spec["chip"],
            "offset": 0,
            "size": target.stat().st_size,
            "sha256": sha256(target),
        }

    shutil.copy(ROOT / "dist" / "catalog.json", out / "catalog.json")
    shutil.copy(licenses, out / "THIRD_PARTY_LICENSES.md")
    manifest = {
        "schema": 1,
        "version": version,
        "flashSize": "4MB",
        "minFlashBytes": 4 * 1024 * 1024,
        "config": {"offset": parts["hncfg"]["offset"], "size": parts["hncfg"]["size"], "schema": 1},
        "images": images,
        "catalog": {"file": "catalog.json", "sha256": sha256(out / "catalog.json")},
        "licenses": {"file": "THIRD_PARTY_LICENSES.md", "sha256": sha256(out / "THIRD_PARTY_LICENSES.md")},
        "source": f"https://github.com/TexhFexLabs/hydronode-firmware/tree/v{version}",
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    sums = [f"{sha256(p)}  {p.name}" for p in sorted(out.iterdir())]
    (out / "SHA256SUMS").write_text("\n".join(sums) + "\n")

    archive = ROOT / "dist" / f"hydronode-firmware-{version}.zip"
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(out.iterdir()):
            z.write(p, p.name)
    print(f"release {version}: {len(images)} images → {archive} (sha256 {sha256(archive)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
