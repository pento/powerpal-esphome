#!/usr/bin/env python3
"""One-off import of Powerpal historical readings into Home Assistant.

Pulls every reading the Powerpal cloud has for the device, buckets it
hourly, and pushes it into HA's long-term statistics for the live
ESPHome sensor's `statistic_id`. Re-running is safe — the import call
is idempotent at the (statistic_id, hour) granularity and replaces
existing rows.

Setup:
  Requires `uv` (https://docs.astral.sh/uv/). Dependencies are
  declared inline below (PEP 723); `uv run` resolves them into a
  cached ephemeral venv.

  POWERPAL_API_KEY=... POWERPAL_DEVICE_ID=... \\
  HA_URL=http://homeassistant.local:8123 \\
  HA_TOKEN=... \\
  STATISTIC_ID=sensor.powerpal_total_energy \\
  uv run scripts/import_history.py

The first run with --dry-run prints what would be imported without
touching HA — recommended for a sanity check.
"""

# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "httpx",
#   "websockets",
# ]
# ///

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
from datetime import datetime, timezone

import httpx
import websockets

POWERPAL_API_BASE = "https://readings.powerpal.net/api/v1"
# Powerpal's hard limit is 50,000 records per response. At sample=60
# (hourly), one request covers ~5.7 years; a single call is enough for
# any realistic install.
POWERPAL_MAX_RECORDS = 50_000


async def fetch_powerpal_history(api_key: str, device_id: str) -> list[dict]:
    """Fetch the device's full hourly history from the Powerpal cloud."""
    headers = {"Authorization": api_key, "Accept-Encoding": "gzip"}
    async with httpx.AsyncClient(headers=headers, timeout=60.0) as client:
        device_resp = await client.get(f"{POWERPAL_API_BASE}/device/{device_id}")
        device_resp.raise_for_status()
        device = device_resp.json()
        first = device["first_reading_timestamp"]
        last = device["last_reading_timestamp"]
        print(
            f"Powerpal device {device_id}: history spans "
            f"{datetime.fromtimestamp(first, tz=timezone.utc).isoformat()} "
            f"→ {datetime.fromtimestamp(last, tz=timezone.utc).isoformat()} "
            f"({(last - first) // 86400} days)"
        )

        readings_resp = await client.get(
            f"{POWERPAL_API_BASE}/meter_reading/{device_id}",
            params={"start": first, "end": last, "sample": 60},
        )
        readings_resp.raise_for_status()
        rows = readings_resp.json()
        if len(rows) >= POWERPAL_MAX_RECORDS:
            sys.exit(
                f"Powerpal returned {len(rows)} records — at or above the "
                f"{POWERPAL_MAX_RECORDS}-record limit. Need to chunk; not "
                f"implemented."
            )
        print(f"Fetched {len(rows)} hourly readings from Powerpal cloud")
        return rows


def to_ha_statistics(rows: list[dict]) -> list[dict]:
    """Convert Powerpal hourly Wh rows to HA cumulative-kWh statistics."""
    running_kwh = 0.0
    stats = []
    for row in rows:
        running_kwh += row["watt_hours"] / 1000.0
        # HA expects ISO-8601 timestamps aligned to hour boundaries.
        ts = datetime.fromtimestamp(row["timestamp"], tz=timezone.utc).replace(
            minute=0, second=0, microsecond=0
        )
        stats.append({
            "start": ts.isoformat(),
            "sum": round(running_kwh, 6),
        })
    return stats


async def import_to_ha(
    ha_url: str, ha_token: str, statistic_id: str, stats: list[dict]
) -> None:
    """Push statistics into HA via the recorder/import_statistics WS call."""
    ws_url = ha_url.rstrip("/").replace("http://", "ws://").replace(
        "https://", "wss://"
    ) + "/api/websocket"
    async with websockets.connect(ws_url) as ws:
        # Auth handshake
        msg = json.loads(await ws.recv())
        assert msg["type"] == "auth_required", msg
        await ws.send(json.dumps({"type": "auth", "access_token": ha_token}))
        msg = json.loads(await ws.recv())
        assert msg["type"] == "auth_ok", msg

        # Submit the import
        await ws.send(json.dumps({
            "id": 1,
            "type": "recorder/import_statistics",
            "metadata": {
                "has_mean": False,
                "has_sum": True,
                "name": None,
                "source": "recorder",
                "statistic_id": statistic_id,
                "unit_of_measurement": "kWh",
            },
            "stats": stats,
        }))
        msg = json.loads(await ws.recv())
        if not msg.get("success"):
            sys.exit(f"HA rejected the import: {msg}")
        print(f"Imported {len(stats)} hourly statistics into {statistic_id}")


async def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Fetch + transform but don't push to HA",
    )
    parser.add_argument(
        "--print-stats",
        type=int,
        default=0,
        metavar="N",
        help="Print the first N statistics rows for inspection",
    )
    args = parser.parse_args()

    api_key = os.environ["POWERPAL_API_KEY"]
    device_id = os.environ["POWERPAL_DEVICE_ID"]
    statistic_id = os.environ.get(
        "STATISTIC_ID", "sensor.powerpal_total_energy"
    )

    rows = await fetch_powerpal_history(api_key, device_id)
    stats = to_ha_statistics(rows)
    print(
        f"Cumulative kWh at end of import range: {stats[-1]['sum']:.3f}"
        if stats
        else "No rows returned."
    )
    if args.print_stats:
        for s in stats[: args.print_stats]:
            print(s)

    if args.dry_run:
        print("--dry-run: skipping HA import")
        return

    ha_url = os.environ["HA_URL"]
    ha_token = os.environ["HA_TOKEN"]
    await import_to_ha(ha_url, ha_token, statistic_id, stats)


if __name__ == "__main__":
    asyncio.run(main())
