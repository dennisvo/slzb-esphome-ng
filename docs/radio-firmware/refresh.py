#!/usr/bin/env python3
"""Fetch the SMLIGHT radio-firmware catalog and write a canonicalized snapshot.

Run manually before opening a PR that touches radio-firmware assumptions.
Review the git diff on ``catalog-snapshot.json`` — new ``(chip_id, type, prod)``
tuples are the drift signal. See ``README.md`` in this folder.
"""
from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

CATALOG_URL = (
    "https://updates.smlight.tech/services/api/slzb-06x-ota.php"
    "?type=ZB&format=slzb"
)
SNAPSHOT_PATH = Path(__file__).with_name("catalog-snapshot.json")
MIN_BYTES = 1024  # sanity floor — real catalog is tens of KB


def main() -> int:
    req = urllib.request.Request(
        CATALOG_URL,
        headers={"User-Agent": "slzb-esphome-ng/radio-firmware-refresh"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            body = resp.read()
    except (urllib.error.URLError, TimeoutError) as exc:
        print(f"error: fetch failed: {exc}", file=sys.stderr)
        return 2

    if len(body) < MIN_BYTES:
        print(
            f"error: response is only {len(body)} bytes "
            f"(< {MIN_BYTES}); refusing to overwrite snapshot",
            file=sys.stderr,
        )
        return 2

    try:
        payload = json.loads(body)
    except json.JSONDecodeError as exc:
        print(f"error: response is not valid JSON: {exc}", file=sys.stderr)
        return 2

    SNAPSHOT_PATH.write_text(
        json.dumps(payload, sort_keys=True, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    print(f"wrote {SNAPSHOT_PATH.name} ({SNAPSHOT_PATH.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
