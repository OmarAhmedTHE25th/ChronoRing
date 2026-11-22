#!/usr/bin/env python3
"""End-to-end test for ChronoRing live columnar Python reader & Web Dashboard."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

repo_root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(repo_root / "python"))

from chronoring.dashboard import DashboardServer  # noqa: E402
from chronoring.live_stream import LiveCrngStream, read_crng_columns  # noqa: E402


def main() -> int:
    cli_bin = repo_root / "build" / "chronoring"
    if not cli_bin.exists():
        raise FileNotFoundError(f"Missing CLI binary at {cli_bin}")

    with tempfile.TemporaryDirectory() as tmp:
        crng_path = Path(tmp) / "live_demo.crng"
        env = dict(os.environ)
        env["LD_LIBRARY_PATH"] = f"{repo_root / 'build'}:{env.get('LD_LIBRARY_PATH', '')}"
        subprocess.run(
            [str(cli_bin), "--demo", "4096", str(crng_path)],
            check=True,
            env=env,
            capture_output=True,
            text=True,
        )

        # 1. Verify full & incremental columnar `.crng` parsing
        cols = read_crng_columns(crng_path)
        num_events = len(cols["sequence_id"])
        assert num_events > 0, "Expected non-empty persisted events"
        assert len(cols["energy_mev"]) == num_events
        assert len(cols["timestamp_ns"]) == num_events

        stream = LiveCrngStream(crng_path)
        pages = stream.poll_new_pages()
        assert len(pages) >= 1
        assert stream.events_read == num_events
        assert stream.poll_new_pages() == []

        # 2. Verify DashboardServer live HTTP endpoints
        srv = DashboardServer("127.0.0.1", 0, crng_path)
        srv.start_background()
        base_url = f"http://127.0.0.1:{srv.port}"
        try:
            with urllib.request.urlopen(f"{base_url}/") as resp:
                assert resp.status == 200
                html = resp.read().decode("utf-8")
                assert "ChronoRing Observability & Control Plane" in html

            with urllib.request.urlopen(f"{base_url}/api/metrics") as resp:
                assert resp.status == 200
                metrics = json.loads(resp.read().decode("utf-8"))
                assert metrics["stream"]["events_read"] == num_events
                assert metrics["config"]["generation"] == 1

            with urllib.request.urlopen(f"{base_url}/metrics") as resp:
                assert resp.status == 200
                prom = resp.read().decode("utf-8")
                assert f"chronoring_dashboard_events_read {num_events}" in prom

            with urllib.request.urlopen(f"{base_url}/api/otlp") as resp:
                assert resp.status == 200
                otlp = json.loads(resp.read().decode("utf-8"))
                assert "resourceMetrics" in otlp

            req = urllib.request.Request(
                f"{base_url}/api/config",
                data=json.dumps({"min_energy_mev": 25.0, "batch_size": 512}).encode("utf-8"),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(req) as resp:
                assert resp.status == 200
                updated = json.loads(resp.read().decode("utf-8"))
                assert updated["generation"] == 2
                assert updated["min_energy_mev"] == 25.0
                assert updated["batch_size"] == 512
        finally:
            srv.stop()

    print(f"[PASS] live_stream + dashboard HTTP/OTLP/Prometheus verified ({num_events} events)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
