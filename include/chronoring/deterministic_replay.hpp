#pragma once

#include "chronoring/crypto_integrity.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chronoring {

struct ReplayTranscript {
    std::size_t files_processed{0};
    std::size_t pages_processed{0};
    std::uint64_t total_events{0};
    std::uint64_t passed_events{0};
    Sha256Digest output_digest{};
};

// Replays multi-file `.crng` datasets deterministically in strict canonical order,
// producing a bit-identical SHA-256 transcript digest across backends and runs.
class DeterministicReplayEngine {
public:
    [[nodiscard]] Expected<ReplayTranscript, RingStatus> replay_files(
        const std::vector<std::string>& crng_paths,
        const TriggerConfig& cfg,
        SimdBackend backend = SimdBackend::Auto) const;
};

} // namespace chronoring
