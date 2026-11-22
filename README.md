# ChronoRing

**Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine** (`C++20`, v1.2.0)

ChronoRing is a standalone, zero-allocation C++20 systems engineering library and pipeline designed for ultra-low-latency high-energy physics / detector telemetry ingestion, vectorized SIMD trigger filtering, and zero-copy memory-mapped columnar persistence with optional zero-allocation LZ4 page compression and hardware-accelerated CRC32C verification.

---

## Architecture

```text
  Detector / Producer Thread(s)
        │
        ▼  (Wait-free SPSC / Fan-Out / Vyukov MPSC / POSIX & Win32 SHM IPC)
  ┌──────────────────────────────────────────────────────────┐
  │  64B Cache-Line Aligned EventFrame Ring Buffer           │
  │  - Shadow index caching, span batching, reserve/commit   │
  │  - Adaptive batching & explicit back-pressure signals    │
  └──────────────────────────────────────────────────────────┘
        │  (Adaptive batch pop on NUMA/core-pinned consumer thread)
        ▼
  ┌──────────────────────────────────────────────────────────┐
  │  SIMD Trigger Engine (AVX2 / SSE4.2 / NEON / Scalar)     │
  │  - SoA-first or AoS filtering + reject-reason bitmask    │
  │  - Custom predicate hook & JSON config serialization     │
  └──────────────────────────────────────────────────────────┘
        │
        ├──────────────────────────────┐
        ▼                              ▼
  ┌──────────────────────────┐   ┌───────────────────────────┐
  │ Zero-Copy ColumnarSink   │   │ Lock-Free HDR Latency     │
  │ - In-memory staging page │   │ Histogram (p50/p99/p99.9) │
  │ - Optional LZ4 page comp │   │ + Reject-reason counters  │
  │ - Durable dual-write     │   │ + Prometheus export       │
  │ - Hardware CRC32C + mmap │   └───────────────────────────┘
  └──────────────────────────┘
```

### Key Modules

| Header / Module | Description |
|---|---|
| `include/chronoring/chronoring.hpp` | Umbrella header including all ChronoRing C++20 APIs and C ABI declarations. |
| `include/chronoring/status.hpp` | `RingStatus`, `Expected<T, E>`, `version()`, and lock-free structured `LogCallback` hooks. |
| `include/chronoring/event_frame.hpp` | 64-byte `EventFrame`, `ColumnarPageHeader`, `ColumnProjectionMask`, and 1024-event `ColumnarPage` (`57,408` bytes). |
| `include/chronoring/spsc_ring.hpp` | Wait-free `SpscRingBuffer` (shadow caching, `std::span` batching, zero-copy `try_reserve_push`/`try_peek`) + `SpscFanOut`. |
| `include/chronoring/mpsc_ring.hpp` | Vyukov lock-free `MpscRingBuffer` with adaptive spin/yield/`std::atomic::wait` (`push_wait`/`pop_wait`). |
| `include/chronoring/shm_ring.hpp` | Cross-process shared-memory ring (`shm_open`/`mmap` on POSIX, `CreateFileMapping` on Win32) + `ShmScopeGuard`. |
| `include/chronoring/pcie_cxl_ring.hpp` | Direct PCIe BAR / VFIO / CXL.mem DAX zero-CPU FPGA DMA ring (`PcieCxlRing`, `MockFpgaDmaProducer`). |
| `include/chronoring/quantum_detector.hpp` | Cryogenic & quantum sensor ingestion adapters (`SNSPD`, `TES`, `MKID`, `SQUID`, `IonTrapQubit`) and trigger presets. |
| `include/chronoring/simd_trigger.hpp` | AVX2, SSE4.2, NEON, and scalar kernels, `diagnose_reject_reasons`, custom predicate hooks, and JSON config serialization. |
| `include/chronoring/columnar_sink.hpp` | In-memory staged `ColumnarSink` (projection + optional LZ4 compression) & `ColumnarReader` (time/sequence seeking, CRC32C, offline replay). |
| `include/chronoring/lz4_codec.hpp` | Zero-allocation LZ4-format block encoder/decoder using a fixed 16 KiB stack hash table. |
| `include/chronoring/numa.hpp` | Portable CPU topology queries and core-pinning / NUMA policy helpers. |
| `include/chronoring/latency_histogram.hpp` | Lock-free logarithmic HDR histogram (`< 0.8%` bucket width, `0 ns` to `> 270 s`). |
| `include/chronoring/pipeline.hpp` | End-to-end `TelemetryPipeline`, adaptive batching, back-pressure signals, durable dual-write, JSON config, and Prometheus exporter. |
| `include/chronoring/c_api.h` | Pure C99/C11 ABI (`chronoring_spsc_*`, `chronoring_sink_*`, `chronoring_reader_*`, `chronoring_shm_*`, `chronoring_pipeline_*`). |
| `python/chronoring/__init__.py` | Thin zero-dependency `ctypes` Python bindings built atop `libchronoring_core`. |

---

## On-Disk `.crng` Columnar Format (v1, Little-Endian)

Each `.crng` file is an array of fixed `57,408`-byte, 64-byte aligned `ColumnarPage` frames:

| Byte Offset | Size | Field / Layout |
|---|---|---|
| `0..63` | `64 B` | `ColumnarPageHeader` (`magic = 0x43524E47` `"CRNG"`, `version = 1`, `flags = ColumnProjectionMask`, `event_count`, `capacity = 1024`, `min_timestamp_ns`, `max_timestamp_ns`, `first_sequence_id`, `last_sequence_id`, `data_crc32c`, `header_crc32c`) |
| `64..16447` | `16,384 B` | `timestamp_ns[1024]`, `sequence_id[1024]` (`uint64_t`) |
| `16448..53311` | `36,864 B` | `detector_id`, `channel_id`, `energy_mev`, `timing_ps`, `pos_x_mm`, `pos_y_mm`, `pos_z_mm`, `pileup_score`, `quality_flags` (9 x 4 KB) |
| `53312..57407` | `4,096 B` | `multiplicity[1024]`, `trigger_mask[1024]` (`uint16_t`) |

### Optional Zero-Allocation LZ4 Compression

- **How to enable**: Pass `enable_compression = true` to `ColumnarSink::open(...)`, set `PipelineConfig::enable_compression = true`, or pass `enable_compression = 1` in the C/Python APIs.
- **On-disk flag & framing**: Sets `kPageFlagLz4` (`1 << 8`) in `ColumnarPageHeader::flags` and stores the compressed payload length in `header.compressed_bytes` (bytes `56..59`). `data_crc32c` is computed over the compressed payload, and `ColumnarSink::close()` truncates the file to the exact compressed byte count.
- **Performance impact**: Uses a fixed 16 KiB stack hash table and a pre-allocated scratch buffer sized once in `open()` — **zero heap allocations** on the hot path. Structured detector telemetry typically compresses by **3x–10x** on disk while keeping `ColumnarReader::read_page()` and `replay_filtered()` transparent.

See [docs/DESIGN.md](docs/DESIGN.md) for detailed architecture notes.

---

## Measured Performance (`bench_chronoring`, GCC 13 `-O3`, 8-core x86_64)

| Benchmark | Measured Throughput / Latency | Notes |
|---|---|---|
| `SpscRingBuffer` single-thread push/pop | **198.15 M items/s** (`5.05 ns/op`, `11.81 GiB/s`) | Shadow index caching |
| `SpscRingBuffer` cross-thread vs `std::mutex` + `std::queue` | **14.87 M items/s** vs `6.39 M items/s` | **~4.9x faster** wall-clock (`0.74 ms` vs `3.66 ms`) |
| SIMD SoA Trigger (`ColumnarPage`, AVX2 vs Scalar) | **422.20 M items/s** vs `175.65 M items/s` | **2.40x speedup**, bit-exact |
| Hardware CRC32C (`_mm_crc32_u64`) vs Software Table | **4.97 GiB/s** vs `229.41 MiB/s` | **22.1x speedup** on 57.4 KB page |

---

## Building, Installing & Testing

### Release Build + Install

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Optional CMake flags:
- `-DCHRONORING_BUILD_SHARED=ON` — Build `libchronoring_core.so` / `chronoring_core.dll` (required for Python bindings).
- `-DCHRONORING_BUILD_FUZZERS=ON` — Build `fuzz_rings` and `fuzz_columnar` libFuzzer targets (Clang).
- `-DCHRONORING_ENABLE_ASAN_UBSAN=ON` — Build with AddressSanitizer + UndefinedBehaviorSanitizer.
- `-DCHRONORING_ENABLE_TSAN=ON` — Build with ThreadSanitizer.

Packaging:
- **CPack**: `cpack --config build/CPackConfig.cmake -G "DEB;RPM;TGZ"`
- **vcpkg**: Manifest and port files are provided in `packaging/vcpkg/`.

### Integration Example

```cpp
#include <chronoring/chronoring.hpp>

int main() {
    chronoring::PipelineConfig cfg;
    cfg.output_path = "run_001.crng";
    cfg.use_soa_staging = true;

    chronoring::TelemetryPipeline pipeline(cfg);
    if (!pipeline.start()) return 1;

    chronoring::EventFrame ev{};
    ev.energy_mev = 42.5f;
    ev.quality_flags = chronoring::kFlagValid;
    ev.multiplicity = 2;
    pipeline.submit_wait(ev);

    pipeline.stop();
    return pipeline.last_status() == chronoring::RingStatus::Ok ? 0 : 1;
}
```

### Python Quickstart

Build with `-DCHRONORING_BUILD_SHARED=ON`, then run with `PYTHONPATH=python`:

```python
import chronoring

cfg = chronoring.default_pipeline_config()
cfg.output_path = b"run_001.crng"
cfg.enable_compression = 1
cfg.trigger.min_energy_mev = 10.0

with chronoring.TelemetryPipeline(cfg) as pipe:
    ev = chronoring.EventFrame(energy_mev=42.5, quality_flags=0x1, multiplicity=2)
    pipe.submit_wait(ev)
    pipe.stop()
    st = pipe.stats()
    print(f"Passed={st.passed_events} Pages={st.pages_written}")

with chronoring.ColumnarReader("run_001.crng") as reader:
    assert reader.verify_all()
```

See [python/examples/quickstart.py](python/examples/quickstart.py) for a complete runnable script.

---

## CLI Usage

```bash
# Live telemetry pipeline demo (persists .crng and verifies CRC32C)
./build/chronoring --demo 100000 run.crng

# Offline SoA SIMD replay of a persisted .crng file
./build/chronoring --replay run.crng

# Export Prometheus/OpenTelemetry metrics exposition
./build/chronoring --prometheus 50000

# JSON benchmark & tail-latency report
./build/chronoring --json-report report.json

# Cross-process shared-memory IPC demo
./build/chronoring --shm-producer detector_feed 100000
./build/chronoring --shm-consumer detector_feed 100000

# Generate a complete C++20 DAQ pipeline from a Detector Description Language (.ddl.json) spec
python3 tools/chronoring_ddl.py examples/detectors/cryo_snspd_array.ddl.json --out-dir build/gen_snspd
python3 tools/chronoring_ddl.py examples/detectors/collider_calorimeter.ddl.json --out-dir build/gen_calorimeter
```

---

## Formal Verification

Machine-checked proofs and state-space models live in `formal/`:
- **TLA+ Specifications**: `formal/tla/SpscRing.tla` and `formal/tla/MpscRing.tla` verify shadow-cursor monotonicity, slot mutual exclusion, Vyukov sequence safety, and strict FIFO ordering under TLC.
- **Lean 4 Proofs**: `formal/lean/ChronoRingProofs.lean` provides zero-`sorry` machine-checked proofs of modular ring disjointness and active ticket exclusivity (see [formal/README.md](formal/README.md)).

---

## License

MIT License — see [LICENSE](LICENSE).
