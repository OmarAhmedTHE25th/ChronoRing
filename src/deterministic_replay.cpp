#include "chronoring/deterministic_replay.hpp"
#include "chronoring/columnar_sink.hpp"
#include "chronoring/event_frame.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <memory>

namespace chronoring {

Expected<ReplayTranscript, RingStatus> DeterministicReplayEngine::replay_files(
    const std::vector<std::string>& crng_paths,
    const TriggerConfig& cfg,
    SimdBackend backend) const {
    ReplayTranscript transcript;
    PageMerkleTree page_tree;
    auto page_buf = std::make_unique<ColumnarPage>();

    std::vector<std::uint8_t> digest_stream;
    digest_stream.reserve(4096);

    for (const std::string& path : crng_paths) {
        ColumnarReader reader(path);
        if (!reader.is_open()) {
            return unexpected(reader.last_status() != RingStatus::Ok
                                  ? reader.last_status()
                                  : RingStatus::IoError);
        }
        if (!reader.verify_all_pages()) {
            return unexpected(RingStatus::CorruptedPage);
        }

        for (std::size_t p = 0; p < reader.num_pages(); ++p) {
            if (!reader.read_page(p, *page_buf)) {
                return unexpected(RingStatus::CorruptedPage);
            }
            const std::size_t n =
                std::min<std::size_t>(page_buf->header.event_count, kColumnarPageEvents);
            std::array<std::uint64_t, kColumnarPageEvents / 64u> words{};
            const std::size_t kept =
                evaluate_columnar_page(*page_buf, cfg, words.data(), backend);

            digest_stream.clear();
            const std::size_t word_cnt = (n + 63u) / 64u;
            for (std::size_t w = 0; w < word_cnt; ++w) {
                std::uint64_t m = words[w];
                const std::size_t base = w * 64u;
                while (m != 0u) {
                    const unsigned bit = static_cast<unsigned>(std::countr_zero(m));
                    const std::size_t idx = base + bit;
                    if (idx < n) {
                        std::array<std::uint8_t, 24> row{};
                        std::memcpy(row.data() + 0, &page_buf->timestamp_ns[idx], 8);
                        std::memcpy(row.data() + 8, &page_buf->sequence_id[idx], 8);
                        std::memcpy(row.data() + 16, &page_buf->detector_id[idx], 4);
                        std::memcpy(row.data() + 20, &page_buf->energy_mev[idx], 4);
                        digest_stream.insert(digest_stream.end(), row.begin(), row.end());
                    }
                    m &= (m - 1u);
                }
            }
            const Sha256Digest page_digest =
                sha256(digest_stream.data(), digest_stream.size());
            (void)page_tree.append_leaf_digest(page_digest);

            transcript.pages_processed += 1;
            transcript.total_events += n;
            transcript.passed_events += kept;
        }
        transcript.files_processed += 1;
    }

    transcript.output_digest = page_tree.root();
    return transcript;
}

} // namespace chronoring
