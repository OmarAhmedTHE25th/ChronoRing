"""Thin Python bindings for ChronoRing built atop the C ABI (libchronoring_core)."""

from __future__ import annotations

import ctypes
import os
import sys
from pathlib import Path
from typing import Optional


def _load_library() -> ctypes.CDLL:
    env_path = os.environ.get("CHRONORING_LIB")
    candidates: list[Path] = []
    if env_path:
        candidates.append(Path(env_path))

    if sys.platform.startswith("win"):
        lib_names = ["chronoring_core.dll", "libchronoring_core.dll"]
    elif sys.platform == "darwin":
        lib_names = ["libchronoring_core.dylib"]
    else:
        lib_names = ["libchronoring_core.so"]

    repo_root = Path(__file__).resolve().parents[2]
    for build_dir in [repo_root / "build", repo_root / "build" / "Release", Path.cwd() / "build"]:
        for name in lib_names:
            candidates.append(build_dir / name)

    for cand in candidates:
        if cand.exists():
            return ctypes.CDLL(str(cand))

    for name in lib_names:
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue

    raise RuntimeError(
        "Could not locate chronoring_core shared library. "
        "Build with -DCHRONORING_BUILD_SHARED=ON or set CHRONORING_LIB=/path/to/lib."
    )


_lib = _load_library()


class EventFrame(ctypes.Structure):
    _fields_ = [
        ("timestamp_ns", ctypes.c_uint64),
        ("sequence_id", ctypes.c_uint64),
        ("detector_id", ctypes.c_uint32),
        ("channel_id", ctypes.c_uint32),
        ("energy_mev", ctypes.c_float),
        ("timing_ps", ctypes.c_float),
        ("pos_x_mm", ctypes.c_float),
        ("pos_y_mm", ctypes.c_float),
        ("pos_z_mm", ctypes.c_float),
        ("pileup_score", ctypes.c_float),
        ("quality_flags", ctypes.c_uint32),
        ("multiplicity", ctypes.c_uint16),
        ("trigger_mask", ctypes.c_uint16),
        ("payload_crc", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class TriggerConfig(ctypes.Structure):
    _fields_ = [
        ("min_energy_mev", ctypes.c_float),
        ("max_energy_mev", ctypes.c_float),
        ("max_abs_timing_ps", ctypes.c_float),
        ("max_pileup_score", ctypes.c_float),
        ("required_flags", ctypes.c_uint32),
        ("forbidden_flags", ctypes.c_uint32),
        ("min_multiplicity", ctypes.c_uint16),
        ("required_trigger_mask", ctypes.c_uint16),
    ]


class PipelineConfig(ctypes.Structure):
    _fields_ = [
        ("output_path", ctypes.c_char_p),
        ("trigger", TriggerConfig),
        ("consumer_cpu_core", ctypes.c_int),
        ("batch_size", ctypes.c_size_t),
        ("initial_page_capacity", ctypes.c_size_t),
        ("projection_mask", ctypes.c_uint16),
        ("use_soa_staging", ctypes.c_uint8),
        ("enable_compression", ctypes.c_uint8),
        ("adaptive_batching", ctypes.c_uint8),
        ("reserved_pad", ctypes.c_uint8),
    ]


class PipelineStats(ctypes.Structure):
    _fields_ = [
        ("submitted_events", ctypes.c_uint64),
        ("dropped_events", ctypes.c_uint64),
        ("consumed_events", ctypes.c_uint64),
        ("passed_events", ctypes.c_uint64),
        ("rejected_events", ctypes.c_uint64),
        ("reject_reasons", ctypes.c_uint64 * 5),
        ("pages_written", ctypes.c_uint64),
        ("sink_errors", ctypes.c_uint64),
        ("backpressure_waits", ctypes.c_uint64),
        ("ring_fill_ratio", ctypes.c_double),
        ("elapsed_seconds", ctypes.c_double),
        ("throughput_meps", ctypes.c_double),
        ("min_ns", ctypes.c_uint64),
        ("p50_ns", ctypes.c_uint64),
        ("p90_ns", ctypes.c_uint64),
        ("p99_ns", ctypes.c_uint64),
        ("p99_9_ns", ctypes.c_uint64),
        ("max_ns", ctypes.c_uint64),
        ("mean_ns", ctypes.c_double),
        ("last_status", ctypes.c_uint8),
        ("backpressured", ctypes.c_uint8),
        ("reserved", ctypes.c_uint8 * 6),
    ]


# Bind C ABI signatures
_lib.chronoring_version.restype = ctypes.c_char_p
_lib.chronoring_default_trigger_config.restype = TriggerConfig
_lib.chronoring_default_pipeline_config.restype = PipelineConfig
_lib.chronoring_crc32c.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32]
_lib.chronoring_crc32c.restype = ctypes.c_uint32

_lib.chronoring_sink_create.restype = ctypes.c_void_p
_lib.chronoring_sink_open.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint16, ctypes.c_int
]
_lib.chronoring_sink_open.restype = ctypes.c_int
_lib.chronoring_sink_write_batch.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(EventFrame), ctypes.c_size_t
]
_lib.chronoring_sink_write_batch.restype = ctypes.c_size_t
_lib.chronoring_sink_flush.argtypes = [ctypes.c_void_p]
_lib.chronoring_sink_flush.restype = ctypes.c_int
_lib.chronoring_sink_close.argtypes = [ctypes.c_void_p]
_lib.chronoring_sink_destroy.argtypes = [ctypes.c_void_p]
_lib.chronoring_sink_pages_written.argtypes = [ctypes.c_void_p]
_lib.chronoring_sink_pages_written.restype = ctypes.c_size_t
_lib.chronoring_sink_events_written.argtypes = [ctypes.c_void_p]
_lib.chronoring_sink_events_written.restype = ctypes.c_size_t

_lib.chronoring_reader_open.argtypes = [ctypes.c_char_p]
_lib.chronoring_reader_open.restype = ctypes.c_void_p
_lib.chronoring_reader_destroy.argtypes = [ctypes.c_void_p]
_lib.chronoring_reader_num_pages.argtypes = [ctypes.c_void_p]
_lib.chronoring_reader_num_pages.restype = ctypes.c_size_t
_lib.chronoring_reader_total_events.argtypes = [ctypes.c_void_p]
_lib.chronoring_reader_total_events.restype = ctypes.c_size_t
_lib.chronoring_reader_verify_all.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
_lib.chronoring_reader_verify_all.restype = ctypes.c_int
_lib.chronoring_reader_replay_filtered.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(TriggerConfig), ctypes.POINTER(ctypes.c_uint64)
]
_lib.chronoring_reader_replay_filtered.restype = ctypes.c_int64

_lib.chronoring_pipeline_create.argtypes = [ctypes.POINTER(PipelineConfig)]
_lib.chronoring_pipeline_create.restype = ctypes.c_void_p
_lib.chronoring_pipeline_start.argtypes = [ctypes.c_void_p]
_lib.chronoring_pipeline_start.restype = ctypes.c_int
_lib.chronoring_pipeline_submit.argtypes = [ctypes.c_void_p, ctypes.POINTER(EventFrame)]
_lib.chronoring_pipeline_submit.restype = ctypes.c_int
_lib.chronoring_pipeline_stop.argtypes = [ctypes.c_void_p]
_lib.chronoring_pipeline_stats.argtypes = [ctypes.c_void_p, ctypes.POINTER(PipelineStats)]
_lib.chronoring_pipeline_destroy.argtypes = [ctypes.c_void_p]


def version() -> str:
    return _lib.chronoring_version().decode("utf-8")


def default_trigger_config() -> TriggerConfig:
    return _lib.chronoring_default_trigger_config()


def default_pipeline_config() -> PipelineConfig:
    return _lib.chronoring_default_pipeline_config()


class ColumnarSink:
    def __init__(
        self,
        path: str,
        initial_page_capacity: int = 64,
        projection_mask: int = 0x1F,
        enable_compression: bool = False,
    ) -> None:
        self._handle = _lib.chronoring_sink_create()
        if not self._handle:
            raise MemoryError("Failed to allocate ColumnarSink")
        ok = _lib.chronoring_sink_open(
            self._handle,
            path.encode("utf-8"),
            initial_page_capacity,
            projection_mask,
            1 if enable_compression else 0,
        )
        if not ok:
            _lib.chronoring_sink_destroy(self._handle)
            self._handle = None
            raise OSError(f"Failed to open ColumnarSink: {path}")

    def write_batch(self, frames: list[EventFrame]) -> int:
        if not frames:
            return 0
        arr = (EventFrame * len(frames))(*frames)
        return int(_lib.chronoring_sink_write_batch(self._handle, arr, len(frames)))

    def flush(self) -> bool:
        return bool(_lib.chronoring_sink_flush(self._handle))

    def close(self) -> None:
        if self._handle:
            _lib.chronoring_sink_close(self._handle)
            _lib.chronoring_sink_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "ColumnarSink":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class ColumnarReader:
    def __init__(self, path: str) -> None:
        self._handle = _lib.chronoring_reader_open(path.encode("utf-8"))
        if not self._handle:
            raise OSError(f"Failed to open ColumnarReader: {path}")

    @property
    def num_pages(self) -> int:
        return int(_lib.chronoring_reader_num_pages(self._handle))

    @property
    def total_events(self) -> int:
        return int(_lib.chronoring_reader_total_events(self._handle))

    def verify_all(self) -> bool:
        return bool(_lib.chronoring_reader_verify_all(self._handle, None))

    def replay_filtered(self, cfg: Optional[TriggerConfig] = None) -> tuple[int, int]:
        scanned = ctypes.c_uint64(0)
        cfg_ref = ctypes.byref(cfg) if cfg is not None else None
        passed = _lib.chronoring_reader_replay_filtered(self._handle, cfg_ref, ctypes.byref(scanned))
        if passed < 0:
            raise RuntimeError("ColumnarReader replay failed (corrupted or unreadable page)")
        return int(passed), int(scanned.value)

    def close(self) -> None:
        if self._handle:
            _lib.chronoring_reader_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "ColumnarReader":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


class TelemetryPipeline:
    def __init__(self, cfg: Optional[PipelineConfig] = None) -> None:
        cfg_ref = ctypes.byref(cfg) if cfg is not None else None
        self._handle = _lib.chronoring_pipeline_create(cfg_ref)
        if not self._handle:
            raise MemoryError("Failed to create TelemetryPipeline")

    def start(self) -> bool:
        return bool(_lib.chronoring_pipeline_start(self._handle))

    def submit(self, frame: EventFrame) -> bool:
        return bool(_lib.chronoring_pipeline_submit(self._handle, ctypes.byref(frame)))

    def stop(self) -> None:
        if self._handle:
            _lib.chronoring_pipeline_stop(self._handle)

    def stats(self) -> PipelineStats:
        st = PipelineStats()
        _lib.chronoring_pipeline_stats(self._handle, ctypes.byref(st))
        return st

    def close(self) -> None:
        if self._handle:
            _lib.chronoring_pipeline_stop(self._handle)
            _lib.chronoring_pipeline_destroy(self._handle)
            self._handle = None

    def __enter__(self) -> "TelemetryPipeline":
        if not self.start():
            raise RuntimeError("Failed to start TelemetryPipeline")
        return self

    def __exit__(self, *_: object) -> None:
        self.close()
