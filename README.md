# ChronoRing

**Lock-Free Ring-Buffer Telemetry & SIMD Trigger Engine** (`C++20`)

ChronoRing is a standalone, zero-allocation C++20 systems engineering library and pipeline designed for ultra-low-latency high-energy physics / detector telemetry ingestion, vectorized SIMD trigger filtering, and zero-copy memory-mapped columnar persistence with hardware-accelerated CRC32C verification.

---

## Architecture

```text
  Detector / Producer Thread(s)
        │
        ▼  (Wait-free SPSC / Vyukov MPSC / POSIX & Win32 SHM IPC)
  ┌──────────────────────────────────────────────────────────┐
  │  64B Cache-Line Aligned EventFrame Ring Buffer           │
  └──────────────────────────────────────────────────────────┘
        │  (Batch pop on core-pinned consumer thread)
        ▼
  ┌──────────────────────────────────────────────────────────┐
  │  SIMD Trigger Engine (AVX2 / SSE4.2 / Scalar)            │
  │  - Energy bounds, |timing|, pileup, flags, multiplicity  │
  └──────────────────────────────────────────────────────────┘
        │
        ├──────────────────────────────┐
        ▼                              ▼
  ┌──────────────────────────┐   ┌───────────────────────────┐
  │ Zero-Copy ColumnarSink   │   │ Lock-Free HDR Latency     │
  │ - AoS -> SoA Page (1024) │   │ Histogram (p50/p99/p99.9) │
  │ - Hardware CRC32C        │   └───────────────────────────┘
  │ - mmap Persistence       │
  └──────────────────────────┘
```

### Key Components

| Header / Module | Description |
|---|---|
| `include/chronoring/event_frame.hpp` | Cache-line aligned 64-byte `EventFrame`, `ColumnarPageHeader` (64 B), and zero-padding `ColumnarPage` (1024 events, SoA layout). |
| `include/chronoring/spsc_ring.hpp` | Wait-free Single-Producer Single-Consumer ring buffer with shadow head/tail caching to eliminate cross-core cache-line bouncing. |
| `include/chronoring/mpsc_ring.hpp` | Lock-free Multi-Producer Single-Consumer bounded ring buffer using Vyukov per-slot sequence numbers. |
| `include/chronoring/shm_ring.hpp` | Cross-process shared-memory ring wrapper (`shm_open` + `mmap` on POSIX; `CreateFileMapping` + `MapViewOfFile` on Win32). |
| `include/chronoring/simd_trigger.hpp` | Runtime-dispatched AVX2, SSE4.2, and scalar trigger evaluator with bit-exact equivalence across AoS and SoA layouts. |
| `include/chronoring/columnar_sink.hpp` | Direct mmap AoS-to-SoA page writer (`ColumnarSink`) and verifier (`ColumnarReader`) protected by Castagnoli CRC32C (`_mm_crc32_u64`). |
| `include/chronoring/latency_histogram.hpp` | Lock-free HDR-style logarithmic nanosecond histogram (`< 0.8%` bucket width, `0 ns` to `> 270 s`). |
| `include/chronoring/pipeline.hpp` | End-to-end `TelemetryPipeline` tying ring ingestion, optional CPU core pinning, SIMD filtering, columnar persistence, and latency tracking. |

---

## Building & Testing

### Standard Release Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Sanitizers (ASan + UBSan / ThreadSanitizer)

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_CXX_COMPILER=clang++ -DCHRONORING_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan --parallel
ctest --test-dir build-asan --output-on-failure

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_CXX_COMPILER=clang++ -DCHRONORING_ENABLE_TSAN=ON
cmake --build build-tsan --parallel
ctest --test-dir build-tsan --output-on-failure
```

---

## CLI Usage

```bash
# Run end-to-end telemetry pipeline demo (100k events -> SIMD filter -> mmap CRNG + CRC32C verification)
./build/chronoring --demo 100000

# Emit JSON throughput & tail-latency report
./build/chronoring --json-report report.json

# Cross-process shared-memory IPC demo (run in two terminals)
./build/chronoring --shm-producer detector_feed 100000
./build/chronoring --shm-consumer detector_feed 100000
```

---

## Benchmarks

```bash
./build/bench_chronoring
```

Compares `SpscRingBuffer` and `MpscRingBuffer` against a `std::mutex` + `std::queue` baseline, scalar vs SSE4.2 vs AVX2 trigger evaluation on both AoS and SoA layouts, and software vs hardware SSE4.2 CRC32C page checksums.

---

## License

MIT License — see [LICENSE](LICENSE).
