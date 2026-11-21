#include "chronoring/simd_trigger.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#include <intrin.h>
#define CHRONORING_X86 1
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define CHRONORING_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define CHRONORING_ARM64 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#define CHRONORING_TARGET_AVX2 __attribute__((target("avx2")))
#define CHRONORING_TARGET_SSE42 __attribute__((target("sse4.2")))
#else
#define CHRONORING_TARGET_AVX2
#define CHRONORING_TARGET_SSE42
#endif
#else
#define CHRONORING_TARGET_AVX2
#define CHRONORING_TARGET_SSE42
#endif

namespace chronoring {

namespace {

#if defined(_MSC_VER) && defined(CHRONORING_X86)
bool detect_cpuid_sse42() noexcept {
    int info[4]{0, 0, 0, 0};
    __cpuid(info, 0);
    if (info[0] < 1) {
        return false;
    }
    __cpuidex(info, 1, 0);
    return (info[2] & (1 << 20)) != 0;
}

bool detect_cpuid_avx2() noexcept {
    int info[4]{0, 0, 0, 0};
    __cpuid(info, 0);
    if (info[0] < 7) {
        return false;
    }
    __cpuidex(info, 1, 0);
    const bool has_osxsave = (info[2] & (1 << 27)) != 0;
    const bool has_avx = (info[2] & (1 << 28)) != 0;
    if (!has_osxsave || !has_avx) {
        return false;
    }
    const unsigned long long xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6ULL) != 0x6ULL) {
        return false;
    }
    __cpuidex(info, 7, 0);
    return (info[1] & (1 << 5)) != 0;
}
#endif

void zero_bitmask_words(std::uint64_t* words, std::size_t count) noexcept {
    if (words == nullptr || count == 0) {
        return;
    }
    const std::size_t num_words = (count + 63u) / 64u;
    std::memset(words, 0, num_words * sizeof(std::uint64_t));
}

#ifdef CHRONORING_X86

CHRONORING_TARGET_AVX2
std::uint32_t eval_8_aos_avx2(const EventFrame* p, const TriggerConfig& cfg) noexcept {
    const __m256 e = _mm256_setr_ps(
        p[0].energy_mev, p[1].energy_mev, p[2].energy_mev, p[3].energy_mev,
        p[4].energy_mev, p[5].energy_mev, p[6].energy_mev, p[7].energy_mev);
    const __m256 t = _mm256_setr_ps(
        p[0].timing_ps, p[1].timing_ps, p[2].timing_ps, p[3].timing_ps,
        p[4].timing_ps, p[5].timing_ps, p[6].timing_ps, p[7].timing_ps);
    const __m256 pu = _mm256_setr_ps(
        p[0].pileup_score, p[1].pileup_score, p[2].pileup_score, p[3].pileup_score,
        p[4].pileup_score, p[5].pileup_score, p[6].pileup_score, p[7].pileup_score);

    const __m256 vmin_e = _mm256_set1_ps(cfg.min_energy_mev);
    const __m256 vmax_e = _mm256_set1_ps(cfg.max_energy_mev);
    const __m256 vmax_t = _mm256_set1_ps(cfg.max_abs_timing_ps);
    const __m256 vmax_pu = _mm256_set1_ps(cfg.max_pileup_score);
    const __m256 sign_bit = _mm256_set1_ps(-0.0f);

    const __m256 abs_t = _mm256_andnot_ps(sign_bit, t);
    __m256 fmask = _mm256_and_ps(
        _mm256_cmp_ps(e, vmin_e, _CMP_GE_OQ),
        _mm256_cmp_ps(e, vmax_e, _CMP_LE_OQ));
    fmask = _mm256_and_ps(fmask, _mm256_cmp_ps(abs_t, vmax_t, _CMP_LE_OQ));
    fmask = _mm256_and_ps(fmask, _mm256_cmp_ps(pu, vmax_pu, _CMP_LE_OQ));

    const __m256i qf = _mm256_setr_epi32(
        static_cast<int>(p[0].quality_flags), static_cast<int>(p[1].quality_flags),
        static_cast<int>(p[2].quality_flags), static_cast<int>(p[3].quality_flags),
        static_cast<int>(p[4].quality_flags), static_cast<int>(p[5].quality_flags),
        static_cast<int>(p[6].quality_flags), static_cast<int>(p[7].quality_flags));
    const __m256i mult = _mm256_setr_epi32(
        static_cast<int>(p[0].multiplicity), static_cast<int>(p[1].multiplicity),
        static_cast<int>(p[2].multiplicity), static_cast<int>(p[3].multiplicity),
        static_cast<int>(p[4].multiplicity), static_cast<int>(p[5].multiplicity),
        static_cast<int>(p[6].multiplicity), static_cast<int>(p[7].multiplicity));

    const __m256i zero = _mm256_setzero_si256();
    const __m256i all_ones = _mm256_set1_epi32(-1);
    const __m256i vreq = _mm256_set1_epi32(static_cast<int>(cfg.required_flags));
    const __m256i vforb = _mm256_set1_epi32(static_cast<int>(cfg.forbidden_flags));
    const __m256i vmin_mult = _mm256_set1_epi32(static_cast<int>(cfg.min_multiplicity));

    __m256i imask = _mm256_cmpeq_epi32(_mm256_and_si256(qf, vreq), vreq);
    imask = _mm256_and_si256(imask, _mm256_cmpeq_epi32(_mm256_and_si256(qf, vforb), zero));
    // Unsigned 16-bit values zero-extended to 32-bit are strictly non-negative, so !(min_mult > mult) == (mult >= min_mult).
    const __m256i mult_ok = _mm256_xor_si256(_mm256_cmpgt_epi32(vmin_mult, mult), all_ones);
    imask = _mm256_and_si256(imask, mult_ok);

    if (cfg.required_trigger_mask != 0u) {
        const __m256i tm = _mm256_setr_epi32(
            static_cast<int>(p[0].trigger_mask), static_cast<int>(p[1].trigger_mask),
            static_cast<int>(p[2].trigger_mask), static_cast<int>(p[3].trigger_mask),
            static_cast<int>(p[4].trigger_mask), static_cast<int>(p[5].trigger_mask),
            static_cast<int>(p[6].trigger_mask), static_cast<int>(p[7].trigger_mask));
        const __m256i vreq_tm = _mm256_set1_epi32(static_cast<int>(cfg.required_trigger_mask));
        const __m256i tm_zero = _mm256_cmpeq_epi32(_mm256_and_si256(tm, vreq_tm), zero);
        imask = _mm256_and_si256(imask, _mm256_xor_si256(tm_zero, all_ones));
    }

    const __m256 combined = _mm256_and_ps(fmask, _mm256_castsi256_ps(imask));
    return static_cast<std::uint32_t>(_mm256_movemask_ps(combined));
}

CHRONORING_TARGET_SSE42
std::uint32_t eval_4_aos_sse42(const EventFrame* p, const TriggerConfig& cfg) noexcept {
    const __m128 e = _mm_setr_ps(
        p[0].energy_mev, p[1].energy_mev, p[2].energy_mev, p[3].energy_mev);
    const __m128 t = _mm_setr_ps(
        p[0].timing_ps, p[1].timing_ps, p[2].timing_ps, p[3].timing_ps);
    const __m128 pu = _mm_setr_ps(
        p[0].pileup_score, p[1].pileup_score, p[2].pileup_score, p[3].pileup_score);

    const __m128 vmin_e = _mm_set1_ps(cfg.min_energy_mev);
    const __m128 vmax_e = _mm_set1_ps(cfg.max_energy_mev);
    const __m128 vmax_t = _mm_set1_ps(cfg.max_abs_timing_ps);
    const __m128 vmax_pu = _mm_set1_ps(cfg.max_pileup_score);
    const __m128 sign_bit = _mm_set1_ps(-0.0f);

    const __m128 abs_t = _mm_andnot_ps(sign_bit, t);
    __m128 fmask = _mm_and_ps(_mm_cmpge_ps(e, vmin_e), _mm_cmple_ps(e, vmax_e));
    fmask = _mm_and_ps(fmask, _mm_cmple_ps(abs_t, vmax_t));
    fmask = _mm_and_ps(fmask, _mm_cmple_ps(pu, vmax_pu));

    const __m128i qf = _mm_setr_epi32(
        static_cast<int>(p[0].quality_flags), static_cast<int>(p[1].quality_flags),
        static_cast<int>(p[2].quality_flags), static_cast<int>(p[3].quality_flags));
    const __m128i mult = _mm_setr_epi32(
        static_cast<int>(p[0].multiplicity), static_cast<int>(p[1].multiplicity),
        static_cast<int>(p[2].multiplicity), static_cast<int>(p[3].multiplicity));

    const __m128i zero = _mm_setzero_si128();
    const __m128i all_ones = _mm_set1_epi32(-1);
    const __m128i vreq = _mm_set1_epi32(static_cast<int>(cfg.required_flags));
    const __m128i vforb = _mm_set1_epi32(static_cast<int>(cfg.forbidden_flags));
    const __m128i vmin_mult = _mm_set1_epi32(static_cast<int>(cfg.min_multiplicity));

    __m128i imask = _mm_cmpeq_epi32(_mm_and_si128(qf, vreq), vreq);
    imask = _mm_and_si128(imask, _mm_cmpeq_epi32(_mm_and_si128(qf, vforb), zero));
    const __m128i mult_ok = _mm_xor_si128(_mm_cmpgt_epi32(vmin_mult, mult), all_ones);
    imask = _mm_and_si128(imask, mult_ok);

    if (cfg.required_trigger_mask != 0u) {
        const __m128i tm = _mm_setr_epi32(
            static_cast<int>(p[0].trigger_mask), static_cast<int>(p[1].trigger_mask),
            static_cast<int>(p[2].trigger_mask), static_cast<int>(p[3].trigger_mask));
        const __m128i vreq_tm = _mm_set1_epi32(static_cast<int>(cfg.required_trigger_mask));
        const __m128i tm_zero = _mm_cmpeq_epi32(_mm_and_si128(tm, vreq_tm), zero);
        imask = _mm_and_si128(imask, _mm_xor_si128(tm_zero, all_ones));
    }

    const __m128 combined = _mm_and_ps(fmask, _mm_castsi128_ps(imask));
    return static_cast<std::uint32_t>(_mm_movemask_ps(combined));
}

CHRONORING_TARGET_AVX2
std::uint32_t eval_8_soa_avx2(const ColumnarPage& page, std::size_t idx,
                              const TriggerConfig& cfg) noexcept {
    const __m256 e = _mm256_loadu_ps(page.energy_mev.data() + idx);
    const __m256 t = _mm256_loadu_ps(page.timing_ps.data() + idx);
    const __m256 pu = _mm256_loadu_ps(page.pileup_score.data() + idx);

    const __m256 vmin_e = _mm256_set1_ps(cfg.min_energy_mev);
    const __m256 vmax_e = _mm256_set1_ps(cfg.max_energy_mev);
    const __m256 vmax_t = _mm256_set1_ps(cfg.max_abs_timing_ps);
    const __m256 vmax_pu = _mm256_set1_ps(cfg.max_pileup_score);
    const __m256 sign_bit = _mm256_set1_ps(-0.0f);

    const __m256 abs_t = _mm256_andnot_ps(sign_bit, t);
    __m256 fmask = _mm256_and_ps(
        _mm256_cmp_ps(e, vmin_e, _CMP_GE_OQ),
        _mm256_cmp_ps(e, vmax_e, _CMP_LE_OQ));
    fmask = _mm256_and_ps(fmask, _mm256_cmp_ps(abs_t, vmax_t, _CMP_LE_OQ));
    fmask = _mm256_and_ps(fmask, _mm256_cmp_ps(pu, vmax_pu, _CMP_LE_OQ));

    const __m256i qf = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(page.quality_flags.data() + idx));
    const __m128i mult16 = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(page.multiplicity.data() + idx));
    const __m256i mult = _mm256_cvtepu16_epi32(mult16);

    const __m256i zero = _mm256_setzero_si256();
    const __m256i all_ones = _mm256_set1_epi32(-1);
    const __m256i vreq = _mm256_set1_epi32(static_cast<int>(cfg.required_flags));
    const __m256i vforb = _mm256_set1_epi32(static_cast<int>(cfg.forbidden_flags));
    const __m256i vmin_mult = _mm256_set1_epi32(static_cast<int>(cfg.min_multiplicity));

    __m256i imask = _mm256_cmpeq_epi32(_mm256_and_si256(qf, vreq), vreq);
    imask = _mm256_and_si256(imask, _mm256_cmpeq_epi32(_mm256_and_si256(qf, vforb), zero));
    const __m256i mult_ok = _mm256_xor_si256(_mm256_cmpgt_epi32(vmin_mult, mult), all_ones);
    imask = _mm256_and_si256(imask, mult_ok);

    if (cfg.required_trigger_mask != 0u) {
        const __m128i tm16 = _mm_loadu_si128(
            reinterpret_cast<const __m128i*>(page.trigger_mask.data() + idx));
        const __m256i tm = _mm256_cvtepu16_epi32(tm16);
        const __m256i vreq_tm = _mm256_set1_epi32(static_cast<int>(cfg.required_trigger_mask));
        const __m256i tm_zero = _mm256_cmpeq_epi32(_mm256_and_si256(tm, vreq_tm), zero);
        imask = _mm256_and_si256(imask, _mm256_xor_si256(tm_zero, all_ones));
    }

    const __m256 combined = _mm256_and_ps(fmask, _mm256_castsi256_ps(imask));
    return static_cast<std::uint32_t>(_mm256_movemask_ps(combined));
}

CHRONORING_TARGET_SSE42
std::uint32_t eval_4_soa_sse42(const ColumnarPage& page, std::size_t idx,
                               const TriggerConfig& cfg) noexcept {
    const __m128 e = _mm_loadu_ps(page.energy_mev.data() + idx);
    const __m128 t = _mm_loadu_ps(page.timing_ps.data() + idx);
    const __m128 pu = _mm_loadu_ps(page.pileup_score.data() + idx);

    const __m128 vmin_e = _mm_set1_ps(cfg.min_energy_mev);
    const __m128 vmax_e = _mm_set1_ps(cfg.max_energy_mev);
    const __m128 vmax_t = _mm_set1_ps(cfg.max_abs_timing_ps);
    const __m128 vmax_pu = _mm_set1_ps(cfg.max_pileup_score);
    const __m128 sign_bit = _mm_set1_ps(-0.0f);

    const __m128 abs_t = _mm_andnot_ps(sign_bit, t);
    __m128 fmask = _mm_and_ps(_mm_cmpge_ps(e, vmin_e), _mm_cmple_ps(e, vmax_e));
    fmask = _mm_and_ps(fmask, _mm_cmple_ps(abs_t, vmax_t));
    fmask = _mm_and_ps(fmask, _mm_cmple_ps(pu, vmax_pu));

    const __m128i qf = _mm_loadu_si128(
        reinterpret_cast<const __m128i*>(page.quality_flags.data() + idx));
    const __m128i mult16 = _mm_loadl_epi64(
        reinterpret_cast<const __m128i*>(page.multiplicity.data() + idx));
    const __m128i mult = _mm_cvtepu16_epi32(mult16);

    const __m128i zero = _mm_setzero_si128();
    const __m128i all_ones = _mm_set1_epi32(-1);
    const __m128i vreq = _mm_set1_epi32(static_cast<int>(cfg.required_flags));
    const __m128i vforb = _mm_set1_epi32(static_cast<int>(cfg.forbidden_flags));
    const __m128i vmin_mult = _mm_set1_epi32(static_cast<int>(cfg.min_multiplicity));

    __m128i imask = _mm_cmpeq_epi32(_mm_and_si128(qf, vreq), vreq);
    imask = _mm_and_si128(imask, _mm_cmpeq_epi32(_mm_and_si128(qf, vforb), zero));
    const __m128i mult_ok = _mm_xor_si128(_mm_cmpgt_epi32(vmin_mult, mult), all_ones);
    imask = _mm_and_si128(imask, mult_ok);

    if (cfg.required_trigger_mask != 0u) {
        const __m128i tm16 = _mm_loadl_epi64(
            reinterpret_cast<const __m128i*>(page.trigger_mask.data() + idx));
        const __m128i tm = _mm_cvtepu16_epi32(tm16);
        const __m128i vreq_tm = _mm_set1_epi32(static_cast<int>(cfg.required_trigger_mask));
        const __m128i tm_zero = _mm_cmpeq_epi32(_mm_and_si128(tm, vreq_tm), zero);
        imask = _mm_and_si128(imask, _mm_xor_si128(tm_zero, all_ones));
    }

    const __m128 combined = _mm_and_ps(fmask, _mm_castsi128_ps(imask));
    return static_cast<std::uint32_t>(_mm_movemask_ps(combined));
}

#endif // CHRONORING_X86

bool evaluate_columnar_one(const ColumnarPage& page, std::size_t idx,
                           const TriggerConfig& cfg) noexcept {
    const float energy = page.energy_mev[idx];
    if (!(energy >= cfg.min_energy_mev && energy <= cfg.max_energy_mev)) {
        return false;
    }
    if (!(std::fabs(page.timing_ps[idx]) <= cfg.max_abs_timing_ps)) {
        return false;
    }
    if (!(page.pileup_score[idx] <= cfg.max_pileup_score)) {
        return false;
    }
    const std::uint32_t flags = page.quality_flags[idx];
    if ((flags & cfg.required_flags) != cfg.required_flags) {
        return false;
    }
    if ((flags & cfg.forbidden_flags) != 0u) {
        return false;
    }
    if (page.multiplicity[idx] < cfg.min_multiplicity) {
        return false;
    }
    if (cfg.required_trigger_mask != 0u &&
        (page.trigger_mask[idx] & cfg.required_trigger_mask) == 0u) {
        return false;
    }
    return true;
}

} // namespace

bool cpu_supports_avx2() noexcept {
#if defined(_MSC_VER) && defined(CHRONORING_X86)
    static const bool supported = detect_cpuid_avx2();
    return supported;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(CHRONORING_X86)
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") != 0;
    }();
    return supported;
#else
    return false;
#endif
}

bool cpu_supports_sse42() noexcept {
#if defined(_MSC_VER) && defined(CHRONORING_X86)
    static const bool supported = detect_cpuid_sse42();
    return supported;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(CHRONORING_X86)
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("sse4.2") != 0;
    }();
    return supported;
#else
    return false;
#endif
}

bool cpu_supports_neon() noexcept {
#ifdef CHRONORING_ARM64
    return true;
#else
    return false;
#endif
}

SimdBackend active_backend(SimdBackend requested) noexcept {
    switch (requested) {
    case SimdBackend::Scalar:
        return SimdBackend::Scalar;
    case SimdBackend::AVX2:
        if (cpu_supports_avx2()) {
            return SimdBackend::AVX2;
        }
        return cpu_supports_sse42() ? SimdBackend::SSE42 : SimdBackend::Scalar;
    case SimdBackend::SSE42:
        return cpu_supports_sse42() ? SimdBackend::SSE42 : SimdBackend::Scalar;
    case SimdBackend::NEON:
        return cpu_supports_neon() ? SimdBackend::NEON : SimdBackend::Scalar;
    case SimdBackend::Auto:
    default:
        if (cpu_supports_avx2()) {
            return SimdBackend::AVX2;
        }
        if (cpu_supports_sse42()) {
            return SimdBackend::SSE42;
        }
        if (cpu_supports_neon()) {
            return SimdBackend::NEON;
        }
        return SimdBackend::Scalar;
    }
}

const char* backend_name(SimdBackend backend) noexcept {
    switch (backend) {
    case SimdBackend::Auto:
        return "Auto";
    case SimdBackend::Scalar:
        return "Scalar";
    case SimdBackend::SSE42:
        return "SSE4.2";
    case SimdBackend::AVX2:
        return "AVX2";
    case SimdBackend::NEON:
        return "NEON";
    default:
        return "Unknown";
    }
}

std::size_t filter_events_scalar(const EventFrame* in, std::size_t count,
                                 const TriggerConfig& cfg, EventFrame* out) noexcept {
    if (in == nullptr || count == 0) {
        return 0;
    }
    std::size_t passed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out != nullptr) {
                out[passed] = in[i];
            }
            ++passed;
        }
    }
    return passed;
}

std::size_t filter_events_sse42(const EventFrame* in, std::size_t count,
                                const TriggerConfig& cfg, EventFrame* out) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_sse42()) {
        return filter_events_scalar(in, count, cfg, out);
    }
    if (in == nullptr || count == 0) {
        return 0;
    }
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        std::uint32_t mask = eval_4_aos_sse42(in + i, cfg);
        if (out == nullptr) {
            passed += static_cast<std::size_t>(std::popcount(mask));
        } else {
            while (mask != 0u) {
                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                out[passed++] = in[i + bit];
                mask &= (mask - 1u);
            }
        }
    }
    for (; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out != nullptr) {
                out[passed] = in[i];
            }
            ++passed;
        }
    }
    return passed;
#else
    return filter_events_scalar(in, count, cfg, out);
#endif
}

std::size_t filter_events_avx2(const EventFrame* in, std::size_t count,
                               const TriggerConfig& cfg, EventFrame* out) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_avx2()) {
        return filter_events_sse42(in, count, cfg, out);
    }
    if (in == nullptr || count == 0) {
        return 0;
    }
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        std::uint32_t mask = eval_8_aos_avx2(in + i, cfg);
        if (out == nullptr) {
            passed += static_cast<std::size_t>(std::popcount(mask));
        } else if (mask == 0xFFu) {
            std::memcpy(out + passed, in + i, 8 * sizeof(EventFrame));
            passed += 8;
        } else {
            while (mask != 0u) {
                const unsigned bit = static_cast<unsigned>(std::countr_zero(mask));
                out[passed++] = in[i + bit];
                mask &= (mask - 1u);
            }
        }
    }
    for (; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out != nullptr) {
                out[passed] = in[i];
            }
            ++passed;
        }
    }
    return passed;
#else
    return filter_events_scalar(in, count, cfg, out);
#endif
}

std::size_t filter_events(const EventFrame* in, std::size_t count,
                          const TriggerConfig& cfg, EventFrame* out,
                          SimdBackend backend) noexcept {
    switch (active_backend(backend)) {
    case SimdBackend::AVX2:
        return filter_events_avx2(in, count, cfg, out);
    case SimdBackend::SSE42:
        return filter_events_sse42(in, count, cfg, out);
    case SimdBackend::NEON:
    case SimdBackend::Scalar:
    case SimdBackend::Auto:
    default:
        return filter_events_scalar(in, count, cfg, out);
    }
}

std::size_t evaluate_batch_bitmask_scalar(const EventFrame* in, std::size_t count,
                                          const TriggerConfig& cfg,
                                          std::uint64_t* out_bitmask_words) noexcept {
    if (in == nullptr || count == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, count);
    std::size_t passed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
}

std::size_t evaluate_batch_bitmask_sse42(const EventFrame* in, std::size_t count,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_sse42()) {
        return evaluate_batch_bitmask_scalar(in, count, cfg, out_bitmask_words);
    }
    if (in == nullptr || count == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, count);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const std::uint32_t mask = eval_4_aos_sse42(in + i, cfg);
        passed += static_cast<std::size_t>(std::popcount(mask));
        if (out_bitmask_words != nullptr && mask != 0u) {
            out_bitmask_words[i >> 6] |=
                (static_cast<std::uint64_t>(mask) << (i & 63u));
        }
    }
    for (; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
#else
    return evaluate_batch_bitmask_scalar(in, count, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_batch_bitmask_avx2(const EventFrame* in, std::size_t count,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_avx2()) {
        return evaluate_batch_bitmask_sse42(in, count, cfg, out_bitmask_words);
    }
    if (in == nullptr || count == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, count);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const std::uint32_t mask = eval_8_aos_avx2(in + i, cfg);
        passed += static_cast<std::size_t>(std::popcount(mask));
        if (out_bitmask_words != nullptr && mask != 0u) {
            out_bitmask_words[i >> 6] |=
                (static_cast<std::uint64_t>(mask) << (i & 63u));
        }
    }
    for (; i < count; ++i) {
        if (evaluate_scalar_one(in[i], cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
#else
    return evaluate_batch_bitmask_scalar(in, count, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_batch_bitmask(const EventFrame* in, std::size_t count,
                                   const TriggerConfig& cfg,
                                   std::uint64_t* out_bitmask_words,
                                   SimdBackend backend) noexcept {
    switch (active_backend(backend)) {
    case SimdBackend::AVX2:
        return evaluate_batch_bitmask_avx2(in, count, cfg, out_bitmask_words);
    case SimdBackend::SSE42:
        return evaluate_batch_bitmask_sse42(in, count, cfg, out_bitmask_words);
    case SimdBackend::NEON:
    case SimdBackend::Scalar:
    case SimdBackend::Auto:
    default:
        return evaluate_batch_bitmask_scalar(in, count, cfg, out_bitmask_words);
    }
}

std::size_t evaluate_columnar_page_scalar(const ColumnarPage& page,
                                          const TriggerConfig& cfg,
                                          std::uint64_t* out_bitmask_words) noexcept {
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (evaluate_columnar_one(page, i, cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
}

std::size_t evaluate_columnar_page_sse42(const ColumnarPage& page,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_sse42()) {
        return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
    }
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const std::uint32_t mask = eval_4_soa_sse42(page, i, cfg);
        passed += static_cast<std::size_t>(std::popcount(mask));
        if (out_bitmask_words != nullptr && mask != 0u) {
            out_bitmask_words[i >> 6] |=
                (static_cast<std::uint64_t>(mask) << (i & 63u));
        }
    }
    for (; i < n; ++i) {
        if (evaluate_columnar_one(page, i, cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
#else
    return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_columnar_page_avx2(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_avx2()) {
        return evaluate_columnar_page_sse42(page, cfg, out_bitmask_words);
    }
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const std::uint32_t mask = eval_8_soa_avx2(page, i, cfg);
        passed += static_cast<std::size_t>(std::popcount(mask));
        if (out_bitmask_words != nullptr && mask != 0u) {
            out_bitmask_words[i >> 6] |=
                (static_cast<std::uint64_t>(mask) << (i & 63u));
        }
    }
    for (; i < n; ++i) {
        if (evaluate_columnar_one(page, i, cfg)) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
#else
    return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_columnar_page(const ColumnarPage& page,
                                   const TriggerConfig& cfg,
                                   std::uint64_t* out_bitmask_words,
                                   SimdBackend backend) noexcept {
    switch (active_backend(backend)) {
    case SimdBackend::AVX2:
        return evaluate_columnar_page_avx2(page, cfg, out_bitmask_words);
    case SimdBackend::SSE42:
        return evaluate_columnar_page_sse42(page, cfg, out_bitmask_words);
    case SimdBackend::NEON:
    case SimdBackend::Scalar:
    case SimdBackend::Auto:
    default:
        return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
    }
}

} // namespace chronoring
