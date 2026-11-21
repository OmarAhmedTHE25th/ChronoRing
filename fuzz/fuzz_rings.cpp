#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"
#include "chronoring/lz4_codec.hpp"
#include "chronoring/mpsc_ring.hpp"
#include "chronoring/spsc_ring.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // 1. Fuzz CRC32C software/hardware equivalence
    if (chronoring::crc32c_sw(data, size, 0) != chronoring::crc32c_hw(data, size, 0)) {
        __builtin_trap();
    }

    // 2. Fuzz LZ4 decompressor on arbitrary bytes (must never crash or OOB)
    std::array<std::uint8_t, 4096> decomp_buf{};
    (void)chronoring::lz4_decompress(data, size, decomp_buf.data(), decomp_buf.size());

    // 3. Fuzz LZ4 compress -> decompress round-trip
    if (size > 0 && size <= 2048) {
        std::vector<std::uint8_t> comp(chronoring::lz4_compress_bound(size));
        const std::size_t clen =
            chronoring::lz4_compress(data, size, comp.data(), comp.size());
        if (clen > 0) {
            const std::size_t dlen = chronoring::lz4_decompress(
                comp.data(), clen, decomp_buf.data(), decomp_buf.size());
            if (dlen != size || std::memcmp(data, decomp_buf.data(), size) != 0) {
                __builtin_trap();
            }
        }
    }

    // 4. Fuzz SPSC & MPSC push/pop sequences driven by input bytes
    chronoring::SpscRingBuffer<chronoring::EventFrame, 64> spsc;
    chronoring::MpscRingBuffer<chronoring::EventFrame, 64> mpsc;
    for (std::size_t i = 0; i < size; ++i) {
        chronoring::EventFrame f{};
        f.sequence_id = i;
        f.detector_id = data[i];
        if ((data[i] & 1U) != 0U) {
            (void)spsc.try_push_status(f);
            (void)mpsc.try_push_status(f);
        } else {
            chronoring::EventFrame out{};
            (void)spsc.try_pop_status(out);
            (void)mpsc.try_pop_status(out);
        }
    }
    return 0;
}
