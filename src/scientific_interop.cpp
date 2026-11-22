#include "chronoring/scientific_interop.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <sstream>

namespace chronoring {

namespace {

inline constexpr std::size_t kArrowColumnCount = 13;

struct ColumnDesc {
    const char* name;
    const char* arrow_fmt;
    const char* root_type;
};

constexpr std::array<ColumnDesc, kArrowColumnCount> kColumns = {{
    {"timestamp_ns", "L", "ULong64_t"},
    {"sequence_id", "L", "ULong64_t"},
    {"detector_id", "I", "UInt_t"},
    {"channel_id", "I", "UInt_t"},
    {"energy_mev", "f", "Float_t"},
    {"timing_ps", "f", "Float_t"},
    {"pos_x_mm", "f", "Float_t"},
    {"pos_y_mm", "f", "Float_t"},
    {"pos_z_mm", "f", "Float_t"},
    {"pileup_score", "f", "Float_t"},
    {"quality_flags", "I", "UInt_t"},
    {"multiplicity", "S", "UShort_t"},
    {"trigger_mask", "S", "UShort_t"}
}};

struct ArrowSchemaHolder {
    std::array<ChronoArrowSchema, kArrowColumnCount> children{};
    std::array<ChronoArrowSchema*, kArrowColumnCount> child_ptrs{};
};

struct ArrowArrayHolder {
    std::array<const void*, 1> root_buffers{nullptr};
    std::array<std::array<const void*, 2>, kArrowColumnCount> child_buffers{};
    std::array<ChronoArrowArray, kArrowColumnCount> children{};
    std::array<ChronoArrowArray*, kArrowColumnCount> child_ptrs{};
};

void release_child_schema(ChronoArrowSchema* s) noexcept {
    if (s != nullptr) {
        s->release = nullptr;
    }
}

void release_root_schema(ChronoArrowSchema* s) noexcept {
    if (s == nullptr || s->release == nullptr) {
        return;
    }
    auto* holder = static_cast<ArrowSchemaHolder*>(s->private_data);
    delete holder;
    s->private_data = nullptr;
    s->children = nullptr;
    s->n_children = 0;
    s->release = nullptr;
}

void release_child_array(ChronoArrowArray* a) noexcept {
    if (a != nullptr) {
        a->release = nullptr;
    }
}

void release_root_array(ChronoArrowArray* a) noexcept {
    if (a == nullptr || a->release == nullptr) {
        return;
    }
    auto* holder = static_cast<ArrowArrayHolder*>(a->private_data);
    delete holder;
    a->private_data = nullptr;
    a->children = nullptr;
    a->buffers = nullptr;
    a->n_children = 0;
    a->release = nullptr;
}

} // namespace

RingStatus export_columnar_page_to_arrow(const ColumnarPage& page,
                                         ChronoArrowSchema& out_schema,
                                         ChronoArrowArray& out_array) noexcept {
    auto* s_holder = new (std::nothrow) ArrowSchemaHolder();
    auto* a_holder = new (std::nothrow) ArrowArrayHolder();
    if (s_holder == nullptr || a_holder == nullptr) {
        delete s_holder;
        delete a_holder;
        return RingStatus::IoError;
    }

    const std::int64_t len = static_cast<std::int64_t>(
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents));

    const std::array<const void*, kArrowColumnCount> col_ptrs{
        page.timestamp_ns.data(),
        page.sequence_id.data(),
        page.detector_id.data(),
        page.channel_id.data(),
        page.energy_mev.data(),
        page.timing_ps.data(),
        page.pos_x_mm.data(),
        page.pos_y_mm.data(),
        page.pos_z_mm.data(),
        page.pileup_score.data(),
        page.quality_flags.data(),
        page.multiplicity.data(),
        page.trigger_mask.data()};

    for (std::size_t i = 0; i < kArrowColumnCount; ++i) {
        ChronoArrowSchema& cs = s_holder->children[i];
        cs = ChronoArrowSchema{};
        cs.format = kColumns[i].arrow_fmt;
        cs.name = kColumns[i].name;
        cs.release = &release_child_schema;
        s_holder->child_ptrs[i] = &cs;

        a_holder->child_buffers[i][0] = nullptr;
        a_holder->child_buffers[i][1] = col_ptrs[i];

        ChronoArrowArray& ca = a_holder->children[i];
        ca = ChronoArrowArray{};
        ca.length = len;
        ca.null_count = 0;
        ca.offset = 0;
        ca.n_buffers = 2;
        ca.buffers = a_holder->child_buffers[i].data();
        ca.release = &release_child_array;
        a_holder->child_ptrs[i] = &ca;
    }

    out_schema = ChronoArrowSchema{};
    out_schema.format = "+s";
    out_schema.name = "chronoring_page";
    out_schema.n_children = static_cast<std::int64_t>(kArrowColumnCount);
    out_schema.children = s_holder->child_ptrs.data();
    out_schema.release = &release_root_schema;
    out_schema.private_data = s_holder;

    out_array = ChronoArrowArray{};
    out_array.length = len;
    out_array.null_count = 0;
    out_array.offset = 0;
    out_array.n_buffers = 1;
    out_array.buffers = a_holder->root_buffers.data();
    out_array.n_children = static_cast<std::int64_t>(kArrowColumnCount);
    out_array.children = a_holder->child_ptrs.data();
    out_array.release = &release_root_array;
    out_array.private_data = a_holder;

    return RingStatus::Ok;
}

std::string export_hdf5_root_schema_json(const ColumnarPage& page) {
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    std::ostringstream oss;
    oss << "{\n"
        << "  \"format\": \"chronoring_crng_v1\",\n"
        << "  \"event_count\": " << n << ",\n"
        << "  \"min_timestamp_ns\": " << page.header.min_timestamp_ns << ",\n"
        << "  \"max_timestamp_ns\": " << page.header.max_timestamp_ns << ",\n"
        << "  \"parquet_arrow_root_compatible\": true,\n"
        << "  \"hdf5_dataset\": \"/chronoring/events\",\n"
        << "  \"root_ttree\": \"chronoring_events\",\n"
        << "  \"columns\": [\n";
    for (std::size_t i = 0; i < kArrowColumnCount; ++i) {
        oss << "    {\"name\": \"" << kColumns[i].name
            << "\", \"arrow_format\": \"" << kColumns[i].arrow_fmt
            << "\", \"root_branch_type\": \"" << kColumns[i].root_type << "\"}"
            << ((i + 1 < kArrowColumnCount) ? "," : "") << "\n";
    }
    oss << "  ]\n"
        << "}\n";
    return oss.str();
}

} // namespace chronoring
