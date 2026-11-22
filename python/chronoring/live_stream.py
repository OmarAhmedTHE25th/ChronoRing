"""Live columnar `.crng` stream parser with Arrow/Polars/Jupyter integration."""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Any, Iterator

CRNG_MAGIC = 0x43524E47
CRNG_HEADER_STRUCT = struct.Struct("<IHHIIQQQQIIII")
CRNG_PAGE_EVENTS = 1024
CRNG_BODY_BYTES = 57344
CRNG_PAGE_BYTES = 64 + CRNG_BODY_BYTES
CRNG_FLAG_LZ4 = 1 << 8

_COLUMN_NAMES = (
    "timestamp_ns",
    "sequence_id",
    "detector_id",
    "channel_id",
    "energy_mev",
    "timing_ps",
    "pos_x_mm",
    "pos_y_mm",
    "pos_z_mm",
    "pileup_score",
    "quality_flags",
    "multiplicity",
    "trigger_mask",
)


def _lz4_decompress_block(src: bytes, expected_len: int) -> bytearray:
    out = bytearray()
    ip = 0
    n = len(src)
    while ip < n:
        token = src[ip]
        ip += 1
        lit_len = token >> 4
        if lit_len == 15:
            while ip < n:
                s = src[ip]
                ip += 1
                lit_len += s
                if s != 255:
                    break
        if lit_len > 0:
            out.extend(src[ip : ip + lit_len])
            ip += lit_len
        if ip >= n:
            break
        offset = src[ip] | (src[ip + 1] << 8)
        ip += 2
        if offset == 0 or offset > len(out):
            raise ValueError("Corrupted LZ4 offset in .crng page")
        match_len = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while ip < n:
                s = src[ip]
                ip += 1
                match_len += s
                if s != 255:
                    break
        for _ in range(match_len):
            out.append(out[-offset])
    if len(out) != expected_len:
        raise ValueError(f"Unexpected LZ4 decompressed size {len(out)} != {expected_len}")
    return out


def unpack_soa_body(body: bytes | bytearray, event_count: int) -> dict[str, list[Any]]:
    cnt = min(event_count, CRNG_PAGE_EVENTS)
    return {
        "timestamp_ns": list(struct.unpack_from(f"<{cnt}Q", body, 0)),
        "sequence_id": list(struct.unpack_from(f"<{cnt}Q", body, 8192)),
        "detector_id": list(struct.unpack_from(f"<{cnt}I", body, 16384)),
        "channel_id": list(struct.unpack_from(f"<{cnt}I", body, 20480)),
        "energy_mev": list(struct.unpack_from(f"<{cnt}f", body, 24576)),
        "timing_ps": list(struct.unpack_from(f"<{cnt}f", body, 28672)),
        "pos_x_mm": list(struct.unpack_from(f"<{cnt}f", body, 32768)),
        "pos_y_mm": list(struct.unpack_from(f"<{cnt}f", body, 36864)),
        "pos_z_mm": list(struct.unpack_from(f"<{cnt}f", body, 40960)),
        "pileup_score": list(struct.unpack_from(f"<{cnt}f", body, 45056)),
        "quality_flags": list(struct.unpack_from(f"<{cnt}I", body, 49152)),
        "multiplicity": list(struct.unpack_from(f"<{cnt}H", body, 53248)),
        "trigger_mask": list(struct.unpack_from(f"<{cnt}H", body, 55296)),
    }


def parse_crng_bytes(data: bytes, start_offset: int = 0) -> tuple[list[dict[str, list[Any]]], int]:
    pages: list[dict[str, list[Any]]] = []
    offset = start_offset
    total_len = len(data)
    while offset + 64 <= total_len:
        (
            magic,
            version,
            flags,
            event_count,
            capacity,
            _min_ts,
            _max_ts,
            _first_seq,
            _last_seq,
            _data_crc,
            _hdr_crc,
            compressed_bytes,
            _proj_mask,
        ) = CRNG_HEADER_STRUCT.unpack_from(data, offset)
        if magic != CRNG_MAGIC or version != 1 or capacity != CRNG_PAGE_EVENTS:
            break
        if (flags & CRNG_FLAG_LZ4) != 0:
            if compressed_bytes == 0 or offset + 64 + compressed_bytes > total_len:
                break
            comp_slice = data[offset + 64 : offset + 64 + compressed_bytes]
            body = _lz4_decompress_block(comp_slice, CRNG_BODY_BYTES)
            offset += 64 + compressed_bytes
        else:
            if offset + CRNG_PAGE_BYTES > total_len:
                break
            body = data[offset + 64 : offset + CRNG_PAGE_BYTES]
            offset += CRNG_PAGE_BYTES
        pages.append(unpack_soa_body(body, event_count))
    return pages, offset


def read_crng_columns(path: str | Path) -> dict[str, list[Any]]:
    raw = Path(path).read_bytes()
    pages, _ = parse_crng_bytes(raw, 0)
    merged: dict[str, list[Any]] = {col: [] for col in _COLUMN_NAMES}
    for pg in pages:
        for col in _COLUMN_NAMES:
            merged[col].extend(pg[col])
    return merged


class LiveCrngStream:
    """Incremental zero-copy tailer reading committed pages from a growing `.crng` file."""

    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)
        self.offset = 0
        self.pages_read = 0
        self.events_read = 0

    def poll_new_pages(self) -> list[dict[str, list[Any]]]:
        if not self.path.exists():
            return []
        raw = self.path.read_bytes()
        if len(raw) <= self.offset:
            return []
        pages, next_offset = parse_crng_bytes(raw, self.offset)
        self.offset = next_offset
        self.pages_read += len(pages)
        for pg in pages:
            self.events_read += len(pg["sequence_id"])
        return pages

    def iter_available(self) -> Iterator[dict[str, list[Any]]]:
        yield from self.poll_new_pages()


def to_pyarrow_table(columns: dict[str, list[Any]]) -> Any:
    """Convert ChronoRing columnar dict into a `pyarrow.Table` (zero intermediate row objects)."""
    try:
        import pyarrow as pa  # type: ignore
    except ImportError as exc:
        raise ImportError(
            "pyarrow is required for Arrow conversion (`pip install pyarrow`)"
        ) from exc
    return pa.table(columns)


def to_polars_dataframe(columns: dict[str, list[Any]]) -> Any:
    """Convert ChronoRing columnar dict into a `polars.DataFrame` for Jupyter analysis."""
    try:
        import polars as pl  # type: ignore
    except ImportError as exc:
        raise ImportError(
            "polars is required for Polars conversion (`pip install polars`)"
        ) from exc
    return pl.DataFrame(columns)
