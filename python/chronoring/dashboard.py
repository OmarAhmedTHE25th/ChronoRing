"""Real-time ChronoRing Web Dashboard & Live Control-Plane Server."""

from __future__ import annotations

import argparse
import json
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Optional

from chronoring.live_stream import LiveCrngStream


_DASHBOARD_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<title>ChronoRing Live Control Plane</title>
<style>
  :root { --bg: #0b1020; --card: #131b2e; --border: #23304d; --text: #e6edf7; --muted: #8ea0c2; --accent: #38bdf8; --ok: #34d399; --warn: #fbbf24; }
  * { box-sizing: border-box; font-family: ui-sans-serif, system-ui, -apple-system, Segoe UI, sans-serif; }
  body { margin: 0; background: var(--bg); color: var(--text); padding: 24px; }
  h1 { margin: 0 0 4px; font-size: 22px; display: flex; align-items: center; gap: 10px; }
  .sub { color: var(--muted); font-size: 13px; margin-bottom: 20px; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; margin-bottom: 18px; }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 10px; padding: 14px; }
  .label { color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.06em; }
  .val { font-size: 22px; font-weight: 700; margin-top: 6px; color: var(--accent); }
  .panels { display: grid; grid-template-columns: 1.2fr 1fr; gap: 16px; }
  @media (max-width: 900px) { .panels { grid-template-columns: 1fr; } }
  label { display: block; font-size: 12px; color: var(--muted); margin-top: 10px; }
  input, select { width: 100%; margin-top: 4px; padding: 8px; border-radius: 6px; border: 1px solid var(--border); background: #0d1425; color: var(--text); }
  button { margin-top: 14px; background: var(--accent); color: #05101e; border: 0; padding: 9px 14px; border-radius: 6px; font-weight: 700; cursor: pointer; }
  table { width: 100%; border-collapse: collapse; font-size: 12px; margin-top: 8px; }
  th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--border); }
  th { color: var(--muted); }
</style>
</head>
<body>
  <h1>ChronoRing Observability & Control Plane</h1>
  <div class="sub">Real-time SoA columnar stream monitor, OTLP/Prometheus exporter, and zero-drop hot-reload</div>

  <div class="grid">
    <div class="card"><div class="label">Events Persisted</div><div class="val" id="m-events">0</div></div>
    <div class="card"><div class="label">Columnar Pages</div><div class="val" id="m-pages">0</div></div>
    <div class="card"><div class="label">Mean Energy (MeV)</div><div class="val" id="m-energy">0.0</div></div>
    <div class="card"><div class="label">Tuned Batch Size</div><div class="val" id="m-batch">256</div></div>
    <div class="card"><div class="label">Config Generation</div><div class="val" id="m-gen">1</div></div>
  </div>

  <div class="panels">
    <div class="card">
      <div class="label">Live Columnar Stream Tail (Latest Events)</div>
      <table>
        <thead><tr><th>Seq ID</th><th>Detector</th><th>Energy (MeV)</th><th>Timing (ps)</th><th>Flags</th></tr></thead>
        <tbody id="event-rows"></tbody>
      </table>
    </div>
    <div class="card">
      <div class="label">Zero-Drop Live Configuration Reload</div>
      <label>Min Energy (MeV)<input id="c-min-e" type="number" step="0.5" value="5.0" /></label>
      <label>Max Abs Timing (ps)<input id="c-max-t" type="number" step="10" value="250.0" /></label>
      <label>Batch Size<input id="c-batch" type="number" step="16" value="256" /></label>
      <label>LZ4 Compression<select id="c-comp"><option value="false">Disabled</option><option value="true">Enabled</option></select></label>
      <button onclick="reloadConfig()">Publish Live Config</button>
      <div id="cfg-status" class="sub" style="margin-top:8px;">Ready</div>
    </div>
  </div>

<script>
async function refresh() {
  const res = await fetch('/api/metrics');
  const d = await res.json();
  document.getElementById('m-events').textContent = d.stream.events_read;
  document.getElementById('m-pages').textContent = d.stream.pages_read;
  document.getElementById('m-energy').textContent = d.stream.mean_energy_mev.toFixed(2);
  document.getElementById('m-batch').textContent = d.config.batch_size;
  document.getElementById('m-gen').textContent = d.config.generation;
  const tbody = document.getElementById('event-rows');
  tbody.innerHTML = d.recent_events.map(e =>
    `<tr><td>${e.sequence_id}</td><td>${e.detector_id}</td><td>${e.energy_mev.toFixed(2)}</td><td>${e.timing_ps.toFixed(1)}</td><td>0x${e.quality_flags.toString(16)}</td></tr>`
  ).join('');
}
async function reloadConfig() {
  const payload = {
    min_energy_mev: parseFloat(document.getElementById('c-min-e').value),
    max_abs_timing_ps: parseFloat(document.getElementById('c-max-t').value),
    batch_size: parseInt(document.getElementById('c-batch').value, 10),
    enable_compression: document.getElementById('c-comp').value === 'true'
  };
  const res = await fetch('/api/config', {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)});
  const out = await res.json();
  document.getElementById('cfg-status').textContent = `Published generation ${out.generation}`;
  refresh();
}
setInterval(refresh, 1000);
refresh();
</script>
</body>
</html>
"""


class DashboardState:
    def __init__(self, crng_path: Optional[str | Path] = None) -> None:
        self._lock = threading.Lock()
        self._stream = LiveCrngStream(crng_path) if crng_path else None
        self._recent_events: list[dict[str, Any]] = []
        self._total_energy = 0.0
        self._config: dict[str, Any] = {
            "generation": 1,
            "min_energy_mev": 0.5,
            "max_abs_timing_ps": 250.0,
            "batch_size": 256,
            "enable_compression": False,
            "projection_mask": 31,
        }
        self._anomalies_detected = 0
        self._start_time = time.monotonic()

    def _poll_locked(self) -> None:
        if self._stream is None:
            return
        new_pages = self._stream.poll_new_pages()
        for pg in new_pages:
            n = len(pg["sequence_id"])
            if n == 0:
                continue
            self._total_energy += sum(pg["energy_mev"])
            tail_start = max(0, n - 8)
            self._recent_events = [
                {
                    "sequence_id": int(pg["sequence_id"][i]),
                    "detector_id": int(pg["detector_id"][i]),
                    "energy_mev": float(pg["energy_mev"][i]),
                    "timing_ps": float(pg["timing_ps"][i]),
                    "quality_flags": int(pg["quality_flags"][i]),
                }
                for i in range(tail_start, n)
            ]

    def metrics_snapshot(self) -> dict[str, Any]:
        with self._lock:
            self._poll_locked()
            events = self._stream.events_read if self._stream else 0
            pages = self._stream.pages_read if self._stream else 0
            mean_e = (self._total_energy / events) if events > 0 else 0.0
            return {
                "service": "chronoring",
                "uptime_s": round(time.monotonic() - self._start_time, 3),
                "stream": {
                    "pages_read": pages,
                    "events_read": events,
                    "mean_energy_mev": mean_e,
                },
                "config": dict(self._config),
                "anomalies_detected": self._anomalies_detected,
                "recent_events": list(self._recent_events),
            }

    def prometheus_text(self) -> str:
        snap = self.metrics_snapshot()
        return (
            "# HELP chronoring_dashboard_events_read Total live events parsed from .crng stream\n"
            "# TYPE chronoring_dashboard_events_read counter\n"
            f"chronoring_dashboard_events_read {snap['stream']['events_read']}\n"
            "# HELP chronoring_dashboard_pages_read Total pages parsed from .crng stream\n"
            "# TYPE chronoring_dashboard_pages_read counter\n"
            f"chronoring_dashboard_pages_read {snap['stream']['pages_read']}\n"
            "# HELP chronoring_config_generation Active live configuration generation\n"
            "# TYPE chronoring_config_generation counter\n"
            f"chronoring_config_generation {snap['config']['generation']}\n"
            "# HELP chronoring_anomalies_detected_total Streaming anomalies observed\n"
            "# TYPE chronoring_anomalies_detected_total counter\n"
            f"chronoring_anomalies_detected_total {snap['anomalies_detected']}\n"
        )

    def otlp_json(self) -> dict[str, Any]:
        snap = self.metrics_snapshot()
        return {
            "resourceMetrics": [
                {
                    "resource": {
                        "attributes": [
                            {"key": "service.name", "value": {"stringValue": "chronoring"}}
                        ]
                    },
                    "scopeMetrics": [
                        {
                            "scope": {"name": "chronoring.dashboard", "version": "1.2.0"},
                            "metrics": [
                                {
                                    "name": "chronoring.stream.events_read",
                                    "sum": {
                                        "dataPoints": [
                                            {"asInt": str(snap["stream"]["events_read"])}
                                        ]
                                    },
                                },
                                {
                                    "name": "chronoring.config.generation",
                                    "gauge": {
                                        "dataPoints": [
                                            {"asInt": str(snap["config"]["generation"])}
                                        ]
                                    },
                                },
                            ],
                        }
                    ],
                }
            ]
        }

    def update_config(self, patch: dict[str, Any]) -> dict[str, Any]:
        with self._lock:
            for key in (
                "min_energy_mev",
                "max_abs_timing_ps",
                "batch_size",
                "enable_compression",
                "projection_mask",
            ):
                if key in patch:
                    self._config[key] = patch[key]
            self._config["generation"] += 1
            return dict(self._config)


class DashboardServer:
    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 8080,
        crng_path: Optional[str | Path] = None,
    ) -> None:
        self.state = DashboardState(crng_path)
        state_ref = self.state

        class Handler(BaseHTTPRequestHandler):
            def _send(self, code: int, content_type: str, body: bytes) -> None:
                self.send_response(code)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self) -> None:  # noqa: N802
                if self.path == "/":
                    self._send(HTTPStatus.OK, "text/html; charset=utf-8", _DASHBOARD_HTML.encode("utf-8"))
                elif self.path == "/api/metrics":
                    payload = json.dumps(state_ref.metrics_snapshot()).encode("utf-8")
                    self._send(HTTPStatus.OK, "application/json", payload)
                elif self.path == "/metrics":
                    payload = state_ref.prometheus_text().encode("utf-8")
                    self._send(HTTPStatus.OK, "text/plain; version=0.0.4", payload)
                elif self.path == "/api/otlp":
                    payload = json.dumps(state_ref.otlp_json()).encode("utf-8")
                    self._send(HTTPStatus.OK, "application/json", payload)
                else:
                    self._send(HTTPStatus.NOT_FOUND, "text/plain", b"Not Found")

            def do_POST(self) -> None:  # noqa: N802
                if self.path != "/api/config":
                    self._send(HTTPStatus.NOT_FOUND, "text/plain", b"Not Found")
                    return
                length = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(length) if length > 0 else b"{}"
                try:
                    patch = json.loads(raw.decode("utf-8"))
                    if not isinstance(patch, dict):
                        raise ValueError("Expected JSON object")
                except (ValueError, UnicodeDecodeError) as exc:
                    err = json.dumps({"error": str(exc)}).encode("utf-8")
                    self._send(HTTPStatus.BAD_REQUEST, "application/json", err)
                    return
                updated = state_ref.update_config(patch)
                self._send(HTTPStatus.OK, "application/json", json.dumps(updated).encode("utf-8"))

            def log_message(self, format: str, *args: Any) -> None:
                return

        self._httpd = ThreadingHTTPServer((host, port), Handler)
        self._thread: Optional[threading.Thread] = None

    @property
    def port(self) -> int:
        return int(self._httpd.server_address[1])

    def start_background(self) -> None:
        self._thread = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()
        if self._thread is not None:
            self._thread.join(timeout=2.0)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="ChronoRing real-time Web Dashboard & Control Plane")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--crng", default=None, help="Optional .crng columnar file to stream live")
    args = parser.parse_args(argv)

    srv = DashboardServer(args.host, args.port, args.crng)
    print(f"ChronoRing Dashboard listening on http://{args.host}:{srv.port}/")
    try:
        srv._httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
