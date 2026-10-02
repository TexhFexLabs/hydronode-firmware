#!/usr/bin/env python3
"""Fails when an app image takes more than 85 % of its OTA slot (app0 in partitions/hydronode_4mb.csv).

The margin keeps room for the next releases: an image that no longer fits its slot cannot be
updated over the air, and the partition table itself never changes over the air.

  python tools/check-image-size.py esp32c3 [esp32 ...]     (default: every ESP32 env that was built)
"""

import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LIMIT = 0.85


def slot_size() -> int:
    with open(ROOT / "partitions" / "hydronode_4mb.csv") as f:
        for row in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            if row[0].strip() == "app0":
                return int(row[4].strip(), 0)
    sys.exit("partitions/hydronode_4mb.csv has no app0")


def main() -> int:
    envs = sys.argv[1:] or sorted(p.name for p in (ROOT / ".pio" / "build").glob("*") if (p / "firmware.bin").exists())
    slot = slot_size()
    failed = False
    for env in envs:
        if "esp8266" in env:
            print(f"{env:16} skipped (ESP8266: no OTA slot, config updates only)")
            continue
        image = ROOT / ".pio" / "build" / env / "firmware.bin"
        if not image.exists():
            print(f"{env:16} missing {image}")
            failed = True
            continue
        size = image.stat().st_size
        share = size / slot
        ok = share <= LIMIT
        failed |= not ok
        print(f"{env:16} {size:>8} bytes  {share:6.1%} of {slot} {'ok' if ok else f'over {LIMIT:.0%}'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
