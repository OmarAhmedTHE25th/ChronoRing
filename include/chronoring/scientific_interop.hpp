#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/status.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace chronoring {

// Binary-compatible Apache Arrow C Data Interface ABI structs.
struct ChronoArrowSchema {
    const char* format{nullptr};
    const char* name{nullptr};
    const char* metadata{nullptr};
    std::int64_t flags{0};
    std::int64_t n_children{0};
    ChronoArrowSchema** children{nullptr};
    ChronoArrowSchema* dictionary{nullptr};
    void (*release)(ChronoArrowSchema*) noexcept {nullptr};
    void* private_data{nullptr};
};

struct ChronoArrowArray {
    std::int64_t length{0};
    std::int64_t null_count{0};
    std::int64_t offset{0};
    std::int64_t n_buffers{0};
    std::int64_t n_children{0};
    const void** buffers{nullptr};
    ChronoArrowArray** children{nullptr};
    ChronoArrowArray* dictionary{nullptr};
    void (*release)(ChronoArrowArray*) noexcept {nullptr};
    void* private_data{nullptr};
};

// Zero-copy export of a 1024-event (or partial) ColumnarPage into the Apache Arrow
// C Data Interface struct column layout. Child column buffers alias the input page directly.
// Caller must keep `page` alive until `out_array.release(&out_array)` is invoked.
[[nodiscard]] RingStatus export_columnar_page_to_arrow(
    const ColumnarPage& page,
    ChronoArrowSchema& out_schema,
    ChronoArrowArray& out_array) noexcept;

// Generates standardized HDF5 / Parquet / CERN ROOT RDataFrame column schema metadata (JSON).
[[nodiscard]] std::string export_hdf5_root_schema_json(const ColumnarPage& page);

} // namespace chronoring
