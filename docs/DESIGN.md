# ChronoRing Architecture & Design Decisions

This document explains the core architectural choices behind **ChronoRing**.

---

## 1. Cache-Line Aligned `EventFrame` (64 Bytes)

Detector telemetry events frequently travel across CPU cores via ring buffers and DMA/IPC boundaries. Making `EventFrame` exactly `64` bytes with `alignas(64)` ensures:
- **Zero false sharing** between adjacent ring slots on 64-byte x86/ARM cache lines.
- **Single cache-line transfer** per frame across cores.
- **Trivial copyability** (`std::is_trivially_copyable_v<EventFrame>`), enabling direct `memcpy`, shared-memory mapping, and C ABI interoperability (`chronoring_event_frame_t`).

---

## 2. Wait-Free SPSC vs. Vyukov MPSC Ring Buffers

### SPSC (`SpscRingBuffer`)
Uses monotonically advancing `size_t` cursors (`head`, `tail`) masked with `Capacity - 1` and **shadow index caching**:
- The producer keeps `cached_tail` on its own 64-byte cache line and only reads the consumer's `tail` with `memory_order_acquire` when the cached bound looks full.
- The consumer keeps `cached_head` on its own 64-byte cache line and only reads the producer's `head` with `memory_order_acquire` when the cached bound looks empty.
- Result: zero shared cache-line invalidation on the steady-state fast path (`~5 ns/op` single-thread push/pop).

### Vyukov MPSC (`MpscRingBuffer`)
For multi-producer detector front-ends feeding one aggregator/writer thread, ChronoRing implements Dmitry Vyukov's bounded queue with per-slot atomic `sequence` numbers:
- Producers contend only via CAS on `enqueue_pos_` (`memory_order_relaxed`) and publish readiness via `slot.sequence.store(pos + 1, memory_order_release)`.
- The single consumer advances sequentially, guaranteeing per-producer FIFO ordering without mutexes or dynamic nodes.
- For low-power or bursty workloads, `push_wait` and `pop_wait` add adaptive spin -> yield -> C++20 `std::atomic::wait` (futex) parking.

### Cross-Process SHM (`ShmRingBuffer`)
Shares the same 3-cache-line `ShmRingHeader` (`192` bytes) over POSIX `shm_open` + `mmap` or Win32 `CreateFileMapping` + `MapViewOfFile`, keeping process-local shadow cursors so cross-process producers/consumers minimize inter-process cache bouncing.

---

## 3. Why SoA (`ColumnarPage`, 1024 Events) + In-Memory Staging?

- **SIMD Vectorization**: Filtering AoS `EventFrame` records requires strided 64-byte gathers (`~165–178 M events/s`), whereas contiguous 64-byte-aligned SoA arrays in `ColumnarPage` allow contiguous 256-bit AVX2 vector loads (`> 420 M events/s`).
- **Fixed 1024-Event Pages (`57,408` Bytes)**:
  - Each column array (`1024 x 2B = 2048B`, `1024 x 4B = 4096B`, `1024 x 8B = 8192B`) is an exact multiple of 64 bytes, yielding **zero inter-column padding**.
  - Fits cleanly inside L2 cache during construction in `ColumnarSink::staging_page_`.
  - Enables O(pages) time-range and sequence-ID indexing via `min_timestamp_ns`, `max_timestamp_ns`, `first_sequence_id`, and `last_sequence_id` in `ColumnarPageHeader`.
- **In-Memory Staging + Flush on Commit**: `ColumnarSink` accumulates active events in an aligned in-memory `ColumnarPage` before computing CRC32C and writing the completed page into the `mmap` region, isolating partial writes from mapped file pages.

---

## 4. Hardware CRC32C & On-Disk Layout

ChronoRing uses **Castagnoli CRC32C** (`0x82F63B78`, accelerated via SSE4.2 `_mm_crc32_u64` at ~5 GiB/s) rather than Ethernet CRC32 because modern x86/ARM CPUs provide dedicated hardware instructions with strong single/burst error detection.

### On-Disk `.crng` Format (Little-Endian, Version 1)
Each `.crng` file is a sequence of `N` fixed-size `ColumnarPage` records (`57,408` bytes each):
- **Bytes `0..63`**: `ColumnarPageHeader` (`magic = 0x43524E47` `"CRNG"`, `version = 1`, `flags` = `ColumnProjectionMask`, `event_count`, `capacity = 1024`, `min_timestamp_ns`, `max_timestamp_ns`, `first_sequence_id`, `last_sequence_id`, `data_crc32c`, `header_crc32c`, `reserved`).
- **Bytes `64..57407` (uncompressed)**: 13 contiguous, 64-byte-aligned SoA column arrays of length `1024` (unused tail entries zero-filled before CRC32C).
- **LZ4-compressed pages (`flags & kPageFlagLz4`)**: Bytes `64..(64 + compressed_bytes - 1)` hold the raw LZ4-block compressed SoA body; `data_crc32c` is computed over the compressed payload and `header_crc32c` over the 64-byte header.

### On-Disk Versioning Policy

- `kColumnarFormatVersion` (`1`) identifies breaking structural changes to the 64-byte `ColumnarPageHeader` or uncompressed SoA column layout.
- Additive, backward-compatible extensions (such as `kPageFlagLz4` or `projection_mask` metadata in header bytes `56..63`) use `header.flags` bits and keep `version == 1`. Readers reject unknown `version` values with `RingStatus::UnsupportedVersion` or `RingStatus::CorruptedPage`.

## 6. Memory-Order & Threading Contracts

- **`SpscRingBuffer` / `SpscFanOut`**: Strictly single-producer, single-consumer per channel. `head` is published with `memory_order_release` after slot writes; consumer reads `head` with `memory_order_acquire`. Shadow cursors avoid cross-core cache-line invalidation on non-boundary steps. Rings require `std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>`.
- **`MpscRingBuffer`**: Lock-free multi-producer, single-consumer (Vyukov bounded MPMC sequence protocol). Producers claim slots via `fetch_add(1, relaxed)` or CAS loop and publish readiness via `slot.sequence.store(pos + 1, release)`. Single consumer reads `slot.sequence.load(acquire)`.
- **`ShmRingBuffer`**: Cross-process SPSC over POSIX `shm_open` + `mmap` (or Win32 `CreateFileMappingA` + `MapViewOfFile`). Requires `std::atomic<uint64_t>::is_always_lock_free`.
- **`ColumnarSink`**: Single-writer-thread only; external synchronization is required if multiple threads invoke `write`, `flush`, or `close` concurrently.

