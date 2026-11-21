"""Minimal ChronoRing Python bindings quickstart example."""

import tempfile
from pathlib import Path

import chronoring


def main() -> None:
    print(f"ChronoRing version: {chronoring.version()}")

    with tempfile.TemporaryDirectory() as tmp:
        crng_path = Path(tmp) / "events.crng"

        cfg = chronoring.default_pipeline_config()
        cfg.output_path = str(crng_path).encode("utf-8")
        cfg.enable_compression = 1
        cfg.trigger.min_energy_mev = 10.0

        with chronoring.TelemetryPipeline(cfg) as pipe:
            for i in range(500):
                ev = chronoring.EventFrame(
                    timestamp_ns=1_000_000 + i * 50,
                    sequence_id=i,
                    detector_id=i % 4,
                    channel_id=i % 64,
                    energy_mev=25.0 if (i % 2 == 0) else 2.0,
                    timing_ps=12.0,
                    pileup_score=0.05,
                    quality_flags=0x3,
                    multiplicity=2,
                )
                pipe.submit(ev)

            pipe.stop()
            st = pipe.stats()
            print(
                f"Submitted={st.submitted_events} Passed={st.passed_events} "
                f"Rejected={st.rejected_events} Pages={st.pages_written}"
            )

        with chronoring.ColumnarReader(str(crng_path)) as reader:
            print(f"Verified={reader.verify_all()} TotalEvents={reader.total_events}")


if __name__ == "__main__":
    main()
