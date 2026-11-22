#include "chronoring/simd_trigger.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>

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
#ifdef __ARM_FEATURE_SVE2
#include <arm_sve.h>
#define CHRONORING_SVE2 1
#endif
#elif defined(__riscv_vector)
#include <riscv_vector.h>
#define CHRONORING_RVV 1
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define CHRONORING_WASM_SIMD128 1
#endif

#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#define CHRONORING_TARGET_AVX512 __attribute__((target("avx512f,avx512vl,avx512bw")))
#define CHRONORING_TARGET_AVX2 __attribute__((target("avx2")))
#define CHRONORING_TARGET_SSE42 __attribute__((target("sse4.2")))
#else
#define CHRONORING_TARGET_AVX512
#define CHRONORING_TARGET_AVX2
#define CHRONORING_TARGET_SSE42
#endif
#else
#define CHRONORING_TARGET_AVX512
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

bool detect_cpuid_avx512() noexcept {
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
    if ((xcr0 & 0xE6ULL) != 0xE6ULL) {
        return false;
    }
    __cpuidex(info, 7, 0);
    constexpr int kAvx512Mask = (1 << 16) | (1 << 30) | (1 << 31); // F | BW | VL
    return (info[1] & kAvx512Mask) == kAvx512Mask;
}
#endif

void zero_bitmask_words(std::uint64_t* words, std::size_t count) noexcept {
    if (words == nullptr || count == 0) {
        return;
    }
    const std::size_t num_words = (count + 63u) / 64u;
    std::memset(words, 0, num_words * sizeof(std::uint64_t));
}

EventFrame extract_aos_frame(const ColumnarPage& page, std::size_t i) noexcept {
    EventFrame f{};
    f.timestamp_ns = page.timestamp_ns[i];
    f.sequence_id = page.sequence_id[i];
    f.detector_id = page.detector_id[i];
    f.channel_id = page.channel_id[i];
    f.energy_mev = page.energy_mev[i];
    f.timing_ps = page.timing_ps[i];
    f.pos_x_mm = page.pos_x_mm[i];
    f.pos_y_mm = page.pos_y_mm[i];
    f.pos_z_mm = page.pos_z_mm[i];
    f.pileup_score = page.pileup_score[i];
    f.quality_flags = page.quality_flags[i];
    f.multiplicity = page.multiplicity[i];
    f.trigger_mask = page.trigger_mask[i];
    return f;
}

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
    if (cfg.custom_predicate != nullptr) {
        const EventFrame ev = extract_aos_frame(page, idx);
        if (!cfg.custom_predicate(ev, cfg.custom_user_ctx)) {
            return false;
        }
    }
    return true;
}

std::uint32_t filter_mask_with_custom(const EventFrame* base, std::uint32_t mask,
                                      const TriggerConfig& cfg) noexcept {
    if (cfg.custom_predicate == nullptr || mask == 0u) {
        return mask;
    }
    std::uint32_t m = mask;
    while (m != 0u) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(m));
        if (!cfg.custom_predicate(base[bit], cfg.custom_user_ctx)) {
            mask &= ~(std::uint32_t{1} << bit);
        }
        m &= (m - 1u);
    }
    return mask;
}

std::uint32_t filter_soa_mask_with_custom(const ColumnarPage& page, std::size_t idx,
                                          std::uint32_t mask,
                                          const TriggerConfig& cfg) noexcept {
    if (cfg.custom_predicate == nullptr || mask == 0u) {
        return mask;
    }
    std::uint32_t m = mask;
    while (m != 0u) {
        const unsigned bit = static_cast<unsigned>(std::countr_zero(m));
        const EventFrame ev = extract_aos_frame(page, idx + bit);
        if (!cfg.custom_predicate(ev, cfg.custom_user_ctx)) {
            mask &= ~(std::uint32_t{1} << bit);
        }
        m &= (m - 1u);
    }
    return mask;
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
    return filter_mask_with_custom(p, static_cast<std::uint32_t>(_mm256_movemask_ps(combined)), cfg);
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
    return filter_mask_with_custom(p, static_cast<std::uint32_t>(_mm_movemask_ps(combined)), cfg);
}

CHRONORING_TARGET_AVX512
std::uint32_t eval_16_soa_avx512(const ColumnarPage& page, std::size_t idx,
                                 const TriggerConfig& cfg) noexcept {
    const __m512 e = _mm512_loadu_ps(page.energy_mev.data() + idx);
    const __m512 t = _mm512_loadu_ps(page.timing_ps.data() + idx);
    const __m512 pu = _mm512_loadu_ps(page.pileup_score.data() + idx);

    const __m512 vmin_e = _mm512_set1_ps(cfg.min_energy_mev);
    const __m512 vmax_e = _mm512_set1_ps(cfg.max_energy_mev);
    const __m512 vmax_t = _mm512_set1_ps(cfg.max_abs_timing_ps);
    const __m512 vmax_pu = _mm512_set1_ps(cfg.max_pileup_score);
    const __m512i abs_mask = _mm512_set1_epi32(0x7FFFFFFF);
    const __m512 abs_t = _mm512_castsi512_ps(
        _mm512_and_si512(_mm512_castps_si512(t), abs_mask));

    __mmask16 mask = _mm512_cmp_ps_mask(e, vmin_e, _CMP_GE_OQ);
    mask = static_cast<__mmask16>(mask & _mm512_cmp_ps_mask(e, vmax_e, _CMP_LE_OQ));
    mask = static_cast<__mmask16>(mask & _mm512_cmp_ps_mask(abs_t, vmax_t, _CMP_LE_OQ));
    mask = static_cast<__mmask16>(mask & _mm512_cmp_ps_mask(pu, vmax_pu, _CMP_LE_OQ));

    const __m512i qf = _mm512_loadu_si512(
        reinterpret_cast<const void*>(page.quality_flags.data() + idx));
    const __m256i mult16 = _mm256_loadu_si256(
        reinterpret_cast<const __m256i*>(page.multiplicity.data() + idx));
    const __m512i mult = _mm512_cvtepu16_epi32(mult16);

    const __m512i zero = _mm512_setzero_si512();
    const __m512i vreq = _mm512_set1_epi32(static_cast<int>(cfg.required_flags));
    const __m512i vforb = _mm512_set1_epi32(static_cast<int>(cfg.forbidden_flags));
    const __m512i vmin_mult = _mm512_set1_epi32(static_cast<int>(cfg.min_multiplicity));

    mask = static_cast<__mmask16>(
        mask & _mm512_cmpeq_epi32_mask(_mm512_and_si512(qf, vreq), vreq));
    mask = static_cast<__mmask16>(
        mask & _mm512_cmpeq_epi32_mask(_mm512_and_si512(qf, vforb), zero));
    mask = static_cast<__mmask16>(
        mask & _mm512_cmpge_epu32_mask(mult, vmin_mult));

    if (cfg.required_trigger_mask != 0u) {
        const __m256i tm16 = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(page.trigger_mask.data() + idx));
        const __m512i tm = _mm512_cvtepu16_epi32(tm16);
        const __m512i vreq_tm = _mm512_set1_epi32(static_cast<int>(cfg.required_trigger_mask));
        mask = static_cast<__mmask16>(
            mask & _mm512_cmpneq_epi32_mask(_mm512_and_si512(tm, vreq_tm), zero));
    }

    return filter_soa_mask_with_custom(page, idx, static_cast<std::uint32_t>(mask), cfg);
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
    return filter_soa_mask_with_custom(
        page, idx, static_cast<std::uint32_t>(_mm256_movemask_ps(combined)), cfg);
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
    return filter_soa_mask_with_custom(
        page, idx, static_cast<std::uint32_t>(_mm_movemask_ps(combined)), cfg);
}

#endif // CHRONORING_X86

#ifdef CHRONORING_ARM64

inline std::uint32_t neon_movemask_u32(uint32x4_t mask) noexcept {
    static constexpr std::array<std::uint32_t, 4> kLaneWeights = {1u, 2u, 4u, 8u};
    const uint32x4_t weights = vld1q_u32(kLaneWeights.data());
    return vaddvq_u32(vandq_u32(mask, weights));
}

std::uint32_t eval_4_aos_neon(const EventFrame* p, const TriggerConfig& cfg) noexcept {
    const float32x4_t e = {p[0].energy_mev, p[1].energy_mev, p[2].energy_mev, p[3].energy_mev};
    const float32x4_t t = {p[0].timing_ps, p[1].timing_ps, p[2].timing_ps, p[3].timing_ps};
    const float32x4_t pu = {p[0].pileup_score, p[1].pileup_score, p[2].pileup_score, p[3].pileup_score};

    uint32x4_t mask = vandq_u32(
        vcgeq_f32(e, vdupq_n_f32(cfg.min_energy_mev)),
        vcleq_f32(e, vdupq_n_f32(cfg.max_energy_mev)));
    mask = vandq_u32(mask, vcleq_f32(vabsq_f32(t), vdupq_n_f32(cfg.max_abs_timing_ps)));
    mask = vandq_u32(mask, vcleq_f32(pu, vdupq_n_f32(cfg.max_pileup_score)));

    const uint32x4_t qf = {p[0].quality_flags, p[1].quality_flags, p[2].quality_flags, p[3].quality_flags};
    const uint32x4_t mult = {p[0].multiplicity, p[1].multiplicity, p[2].multiplicity, p[3].multiplicity};
    const uint32x4_t vreq = vdupq_n_u32(cfg.required_flags);
    const uint32x4_t vforb = vdupq_n_u32(cfg.forbidden_flags);

    mask = vandq_u32(mask, vceqq_u32(vandq_u32(qf, vreq), vreq));
    mask = vandq_u32(mask, vceqq_u32(vandq_u32(qf, vforb), vdupq_n_u32(0u)));
    mask = vandq_u32(mask, vcgeq_u32(mult, vdupq_n_u32(cfg.min_multiplicity)));

    if (cfg.required_trigger_mask != 0u) {
        const uint32x4_t tm = {p[0].trigger_mask, p[1].trigger_mask, p[2].trigger_mask, p[3].trigger_mask};
        const uint32x4_t hit = vmvnq_u32(
            vceqq_u32(vandq_u32(tm, vdupq_n_u32(cfg.required_trigger_mask)), vdupq_n_u32(0u)));
        mask = vandq_u32(mask, hit);
    }

    return filter_mask_with_custom(p, neon_movemask_u32(mask), cfg);
}

std::uint32_t eval_4_soa_neon(const ColumnarPage& page, std::size_t idx,
                              const TriggerConfig& cfg) noexcept {
    const float32x4_t e = vld1q_f32(page.energy_mev.data() + idx);
    const float32x4_t t = vld1q_f32(page.timing_ps.data() + idx);
    const float32x4_t pu = vld1q_f32(page.pileup_score.data() + idx);

    uint32x4_t mask = vandq_u32(
        vcgeq_f32(e, vdupq_n_f32(cfg.min_energy_mev)),
        vcleq_f32(e, vdupq_n_f32(cfg.max_energy_mev)));
    mask = vandq_u32(mask, vcleq_f32(vabsq_f32(t), vdupq_n_f32(cfg.max_abs_timing_ps)));
    mask = vandq_u32(mask, vcleq_f32(pu, vdupq_n_f32(cfg.max_pileup_score)));

    const uint32x4_t qf = vld1q_u32(page.quality_flags.data() + idx);
    const uint16x4_t mult16 = vld1_u16(page.multiplicity.data() + idx);
    const uint32x4_t mult = vmovl_u16(mult16);
    const uint32x4_t vreq = vdupq_n_u32(cfg.required_flags);
    const uint32x4_t vforb = vdupq_n_u32(cfg.forbidden_flags);

    mask = vandq_u32(mask, vceqq_u32(vandq_u32(qf, vreq), vreq));
    mask = vandq_u32(mask, vceqq_u32(vandq_u32(qf, vforb), vdupq_n_u32(0u)));
    mask = vandq_u32(mask, vcgeq_u32(mult, vdupq_n_u32(cfg.min_multiplicity)));

    if (cfg.required_trigger_mask != 0u) {
        const uint16x4_t tm16 = vld1_u16(page.trigger_mask.data() + idx);
        const uint32x4_t tm = vmovl_u16(tm16);
        const uint32x4_t hit = vmvnq_u32(
            vceqq_u32(vandq_u32(tm, vdupq_n_u32(cfg.required_trigger_mask)), vdupq_n_u32(0u)));
        mask = vandq_u32(mask, hit);
    }

    return filter_soa_mask_with_custom(page, idx, neon_movemask_u32(mask), cfg);
}

#endif // CHRONORING_ARM64

std::string_view trim_ws(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) {
        sv.remove_suffix(1);
    }
    return sv;
}

bool find_json_number_token(std::string_view json, std::string_view key,
                            std::string_view& out_tok) noexcept {
    std::string pattern = "\"";
    pattern.append(key);
    pattern.push_back('"');
    const std::size_t kpos = json.find(pattern);
    if (kpos == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = json.find(':', kpos + pattern.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    std::string_view rest = trim_ws(json.substr(colon + 1));
    std::size_t end = 0;
    while (end < rest.size()) {
        const char ch = rest[end];
        if (ch == ',' || ch == '}' || ch == ']' ||
            std::isspace(static_cast<unsigned char>(ch))) {
            break;
        }
        ++end;
    }
    if (end == 0) {
        return false;
    }
    out_tok = rest.substr(0, end);
    return true;
}

bool parse_float_field(std::string_view json, std::string_view key, float& out) noexcept {
    std::string_view tok;
    if (!find_json_number_token(json, key, tok)) {
        return false;
    }
    std::array<char, 64> buf{};
    if (tok.size() >= buf.size()) {
        return false;
    }
    std::memcpy(buf.data(), tok.data(), tok.size());
    buf[tok.size()] = '\0';
    char* endptr = nullptr;
    const float val = std::strtof(buf.data(), &endptr);
    if (endptr != buf.data() + tok.size()) {
        return false;
    }
    out = val;
    return true;
}

template <typename UInt>
bool parse_uint_field(std::string_view json, std::string_view key, UInt& out) noexcept {
    std::string_view tok;
    if (!find_json_number_token(json, key, tok)) {
        return false;
    }
    UInt val = 0;
    const auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
    if (ec != std::errc{} || ptr != tok.data() + tok.size()) {
        return false;
    }
    out = val;
    return true;
}

} // namespace

bool cpu_supports_avx512() noexcept {
#if defined(_MSC_VER) && defined(CHRONORING_X86)
    static const bool supported = detect_cpuid_avx512();
    return supported;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(CHRONORING_X86)
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx512f") != 0 &&
               __builtin_cpu_supports("avx512vl") != 0 &&
               __builtin_cpu_supports("avx512bw") != 0;
    }();
    return supported;
#else
    return false;
#endif
}

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

bool cpu_supports_sve2() noexcept {
#ifdef CHRONORING_SVE2
    return true;
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

bool cpu_supports_rvv() noexcept {
#ifdef CHRONORING_RVV
    return true;
#else
    return false;
#endif
}

bool cpu_supports_wasm_simd128() noexcept {
#ifdef CHRONORING_WASM_SIMD128
    return true;
#else
    return false;
#endif
}

SimdBackend active_backend(SimdBackend requested) noexcept {
    switch (requested) {
    case SimdBackend::Scalar:
        return SimdBackend::Scalar;
    case SimdBackend::AVX512:
        if (cpu_supports_avx512()) {
            return SimdBackend::AVX512;
        }
        return cpu_supports_avx2() ? SimdBackend::AVX2
                                   : (cpu_supports_sse42() ? SimdBackend::SSE42 : SimdBackend::Scalar);
    case SimdBackend::AVX2:
        if (cpu_supports_avx2()) {
            return SimdBackend::AVX2;
        }
        return cpu_supports_sse42() ? SimdBackend::SSE42 : SimdBackend::Scalar;
    case SimdBackend::SSE42:
        return cpu_supports_sse42() ? SimdBackend::SSE42 : SimdBackend::Scalar;
    case SimdBackend::SVE2:
        if (cpu_supports_sve2()) {
            return SimdBackend::SVE2;
        }
        return cpu_supports_neon() ? SimdBackend::NEON : SimdBackend::Scalar;
    case SimdBackend::NEON:
        return cpu_supports_neon() ? SimdBackend::NEON : SimdBackend::Scalar;
    case SimdBackend::RVV:
        return cpu_supports_rvv() ? SimdBackend::RVV : SimdBackend::Scalar;
    case SimdBackend::WasmSimd128:
        return cpu_supports_wasm_simd128() ? SimdBackend::WasmSimd128 : SimdBackend::Scalar;
    case SimdBackend::Auto:
    default:
        if (cpu_supports_avx512()) {
            return SimdBackend::AVX512;
        }
        if (cpu_supports_avx2()) {
            return SimdBackend::AVX2;
        }
        if (cpu_supports_sse42()) {
            return SimdBackend::SSE42;
        }
        if (cpu_supports_sve2()) {
            return SimdBackend::SVE2;
        }
        if (cpu_supports_neon()) {
            return SimdBackend::NEON;
        }
        if (cpu_supports_rvv()) {
            return SimdBackend::RVV;
        }
        if (cpu_supports_wasm_simd128()) {
            return SimdBackend::WasmSimd128;
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
    case SimdBackend::AVX512:
        return "AVX-512";
    case SimdBackend::SVE2:
        return "SVE2";
    case SimdBackend::NEON:
        return "NEON";
    case SimdBackend::RVV:
        return "RVV";
    case SimdBackend::WasmSimd128:
        return "WASM-SIMD128";
    default:
        return "Unknown";
    }
}

std::size_t diagnose_batch_reject_reasons(const EventFrame* in, std::size_t count,
                                          const TriggerConfig& cfg,
                                          std::uint8_t* out_reasons) noexcept {
    if (in == nullptr || out_reasons == nullptr || count == 0) {
        return 0;
    }
    std::size_t passed = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t r = diagnose_reject_reasons(in[i], cfg);
        out_reasons[i] = r;
        if (r == kRejectNone &&
            (cfg.custom_predicate == nullptr ||
             cfg.custom_predicate(in[i], cfg.custom_user_ctx))) {
            ++passed;
        }
    }
    return passed;
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

std::size_t filter_events_neon(const EventFrame* in, std::size_t count,
                               const TriggerConfig& cfg, EventFrame* out) noexcept {
#ifdef CHRONORING_ARM64
    if (in == nullptr || count == 0) {
        return 0;
    }
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        std::uint32_t mask = eval_4_aos_neon(in + i, cfg);
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

std::size_t filter_events(const EventFrame* in, std::size_t count,
                          const TriggerConfig& cfg, EventFrame* out,
                          SimdBackend backend) noexcept {
    switch (active_backend(backend)) {
    case SimdBackend::AVX512:
    case SimdBackend::AVX2:
        return filter_events_avx2(in, count, cfg, out);
    case SimdBackend::SSE42:
        return filter_events_sse42(in, count, cfg, out);
    case SimdBackend::SVE2:
    case SimdBackend::NEON:
        return filter_events_neon(in, count, cfg, out);
    case SimdBackend::RVV:
    case SimdBackend::WasmSimd128:
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

std::size_t evaluate_batch_bitmask_neon(const EventFrame* in, std::size_t count,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_ARM64
    if (in == nullptr || count == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, count);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const std::uint32_t mask = eval_4_aos_neon(in + i, cfg);
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
        return evaluate_batch_bitmask_neon(in, count, cfg, out_bitmask_words);
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

std::size_t evaluate_columnar_page_neon(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_ARM64
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const std::uint32_t mask = eval_4_soa_neon(page, i, cfg);
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

std::size_t evaluate_columnar_page_avx512(const ColumnarPage& page,
                                           const TriggerConfig& cfg,
                                           std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_X86
    if (!cpu_supports_avx512()) {
        return evaluate_columnar_page_avx2(page, cfg, out_bitmask_words);
    }
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const std::uint32_t mask = eval_16_soa_avx512(page, i, cfg);
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

std::size_t evaluate_columnar_page_sve2(const ColumnarPage& page,
                                         const TriggerConfig& cfg,
                                         std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_SVE2
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    const std::size_t vl = svcntw();
    for (std::size_t i = 0; i < n; i += vl) {
        const svbool_t pg = svwhilelt_b32(i, n);
        const svfloat32_t e = svld1_f32(pg, page.energy_mev.data() + i);
        const svfloat32_t t = svabs_f32_x(pg, svld1_f32(pg, page.timing_ps.data() + i));
        const svfloat32_t pu = svld1_f32(pg, page.pileup_score.data() + i);
        svbool_t ok = svcmpge_n_f32(pg, e, cfg.min_energy_mev);
        ok = svcmple_n_f32(ok, e, cfg.max_energy_mev);
        ok = svcmple_n_f32(ok, t, cfg.max_abs_timing_ps);
        ok = svcmple_n_f32(ok, pu, cfg.max_pileup_score);
        const svuint32_t qf = svld1_u32(pg, page.quality_flags.data() + i);
        const svuint32_t mult = svld1uh_u32(pg, page.multiplicity.data() + i);
        ok = svcmpeq_n_u32(ok, svand_n_u32_z(pg, qf, cfg.required_flags), cfg.required_flags);
        ok = svcmpeq_n_u32(ok, svand_n_u32_z(pg, qf, cfg.forbidden_flags), 0u);
        ok = svcmpge_n_u32(ok, mult, cfg.min_multiplicity);
        if (cfg.required_trigger_mask != 0u) {
            const svuint32_t tm = svld1uh_u32(pg, page.trigger_mask.data() + i);
            ok = svcmpne_n_u32(ok, svand_n_u32_z(pg, tm, cfg.required_trigger_mask), 0u);
        }
        const std::size_t lane_lim = std::min(vl, n - i);
        for (std::size_t l = 0; l < lane_lim; ++l) {
            if (svptest_any(pg, svand_b_z(pg, ok, svwhilelt_b32(l, l + 1))) &&
                (cfg.custom_predicate == nullptr ||
                 cfg.custom_predicate(extract_aos_frame(page, i + l), cfg.custom_user_ctx))) {
                if (out_bitmask_words != nullptr) {
                    out_bitmask_words[(i + l) >> 6] |= (std::uint64_t{1} << ((i + l) & 63u));
                }
                ++passed;
            }
        }
    }
    return passed;
#else
    return evaluate_columnar_page_neon(page, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_columnar_page_rvv(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_RVV
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    for (std::size_t i = 0; i < n;) {
        const std::size_t vl = __riscv_vsetvl_e32m1(n - i);
        const vfloat32m1_t e = __riscv_vle32_v_f32m1(page.energy_mev.data() + i, vl);
        const vfloat32m1_t t = __riscv_vfabs_v_f32m1(
            __riscv_vle32_v_f32m1(page.timing_ps.data() + i, vl), vl);
        const vfloat32m1_t pu = __riscv_vle32_v_f32m1(page.pileup_score.data() + i, vl);
        vbool32_t ok = __riscv_vmfge_vf_f32m1_b32(e, cfg.min_energy_mev, vl);
        ok = __riscv_vmand_mm_b32(ok, __riscv_vmfle_vf_f32m1_b32(e, cfg.max_energy_mev, vl), vl);
        ok = __riscv_vmand_mm_b32(ok, __riscv_vmfle_vf_f32m1_b32(t, cfg.max_abs_timing_ps, vl), vl);
        ok = __riscv_vmand_mm_b32(ok, __riscv_vmfle_vf_f32m1_b32(pu, cfg.max_pileup_score, vl), vl);
        const vuint32m1_t qf = __riscv_vle32_v_u32m1(page.quality_flags.data() + i, vl);
        const vuint32m1_t mult = __riscv_vwcvtu_x_x_v_u32m1(
            __riscv_vle16_v_u16mf2(page.multiplicity.data() + i, vl), vl);
        ok = __riscv_vmand_mm_b32(
            ok, __riscv_vmseq_vx_u32m1_b32(__riscv_vand_vx_u32m1(qf, cfg.required_flags, vl),
                                           cfg.required_flags, vl), vl);
        ok = __riscv_vmand_mm_b32(
            ok, __riscv_vmseq_vx_u32m1_b32(__riscv_vand_vx_u32m1(qf, cfg.forbidden_flags, vl),
                                           0u, vl), vl);
        ok = __riscv_vmand_mm_b32(
            ok, __riscv_vmsgeu_vx_u32m1_b32(mult, cfg.min_multiplicity, vl), vl);
        if (cfg.required_trigger_mask != 0u) {
            const vuint32m1_t tm = __riscv_vwcvtu_x_x_v_u32m1(
                __riscv_vle16_v_u16mf2(page.trigger_mask.data() + i, vl), vl);
            ok = __riscv_vmand_mm_b32(
                ok, __riscv_vmsne_vx_u32m1_b32(
                        __riscv_vand_vx_u32m1(tm, cfg.required_trigger_mask, vl), 0u, vl), vl);
        }
        const unsigned long cnt = __riscv_vcpop_m_b32(ok, vl);
        if (cnt > 0) {
            for (std::size_t l = 0; l < vl; ++l) {
                if (evaluate_columnar_one(page, i + l, cfg)) {
                    if (out_bitmask_words != nullptr) {
                        out_bitmask_words[(i + l) >> 6] |= (std::uint64_t{1} << ((i + l) & 63u));
                    }
                    ++passed;
                }
            }
        }
        i += vl;
    }
    return passed;
#else
    return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
#endif
}

std::size_t evaluate_columnar_page_wasm_simd128(const ColumnarPage& page,
                                                 const TriggerConfig& cfg,
                                                 std::uint64_t* out_bitmask_words) noexcept {
#ifdef CHRONORING_WASM_SIMD128
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (n == 0) {
        return 0;
    }
    zero_bitmask_words(out_bitmask_words, n);
    std::size_t passed = 0;
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const v128_t e = wasm_v128_load(page.energy_mev.data() + i);
        const v128_t t = wasm_f32x4_abs(wasm_v128_load(page.timing_ps.data() + i));
        const v128_t pu = wasm_v128_load(page.pileup_score.data() + i);
        v128_t m = wasm_v128_and(
            wasm_f32x4_ge(e, wasm_f32x4_splat(cfg.min_energy_mev)),
            wasm_f32x4_le(e, wasm_f32x4_splat(cfg.max_energy_mev)));
        m = wasm_v128_and(m, wasm_f32x4_le(t, wasm_f32x4_splat(cfg.max_abs_timing_ps)));
        m = wasm_v128_and(m, wasm_f32x4_le(pu, wasm_f32x4_splat(cfg.max_pileup_score)));
        const std::uint32_t fbits = static_cast<std::uint32_t>(wasm_i32x4_bitmask(m));
        for (std::size_t l = 0; l < 4; ++l) {
            if ((fbits & (1u << l)) != 0u && evaluate_columnar_one(page, i + l, cfg)) {
                if (out_bitmask_words != nullptr) {
                    out_bitmask_words[(i + l) >> 6] |= (std::uint64_t{1} << ((i + l) & 63u));
                }
                ++passed;
            }
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
    case SimdBackend::AVX512:
        return evaluate_columnar_page_avx512(page, cfg, out_bitmask_words);
    case SimdBackend::AVX2:
        return evaluate_columnar_page_avx2(page, cfg, out_bitmask_words);
    case SimdBackend::SSE42:
        return evaluate_columnar_page_sse42(page, cfg, out_bitmask_words);
    case SimdBackend::SVE2:
        return evaluate_columnar_page_sve2(page, cfg, out_bitmask_words);
    case SimdBackend::NEON:
        return evaluate_columnar_page_neon(page, cfg, out_bitmask_words);
    case SimdBackend::RVV:
        return evaluate_columnar_page_rvv(page, cfg, out_bitmask_words);
    case SimdBackend::WasmSimd128:
        return evaluate_columnar_page_wasm_simd128(page, cfg, out_bitmask_words);
    case SimdBackend::Scalar:
    case SimdBackend::Auto:
    default:
        return evaluate_columnar_page_scalar(page, cfg, out_bitmask_words);
    }
}

std::size_t compact_columnar_page_to_aos(const ColumnarPage& page,
                                         const std::uint64_t* bitmask_words,
                                         EventFrame* out) noexcept {
    if (bitmask_words == nullptr || out == nullptr) {
        return 0;
    }
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    const std::size_t num_words = (n + 63u) / 64u;
    std::size_t written = 0;

    for (std::size_t w = 0; w < num_words; ++w) {
        std::uint64_t word = bitmask_words[w];
        const std::size_t base = w * 64u;
        while (word != 0u) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(word));
            const std::size_t idx = base + bit;
            if (idx < n) {
                out[written++] = extract_aos_frame(page, idx);
            }
            word &= (word - 1u);
        }
    }
    return written;
}

std::size_t filter_columnar_page_to_aos(const ColumnarPage& page,
                                        const TriggerConfig& cfg,
                                        EventFrame* out,
                                        SimdBackend backend) noexcept {
    std::array<std::uint64_t, kColumnarPageEvents / 64u> words{};
    const std::size_t count = evaluate_columnar_page(page, cfg, words.data(), backend);
    if (out == nullptr || count == 0) {
        return count;
    }
    return compact_columnar_page_to_aos(page, words.data(), out);
}

std::string to_json(const TriggerConfig& cfg) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(4)
        << "{\n"
        << "  \"min_energy_mev\": " << cfg.min_energy_mev << ",\n"
        << "  \"max_energy_mev\": " << cfg.max_energy_mev << ",\n"
        << "  \"max_abs_timing_ps\": " << cfg.max_abs_timing_ps << ",\n"
        << "  \"max_pileup_score\": " << cfg.max_pileup_score << ",\n"
        << "  \"required_flags\": " << cfg.required_flags << ",\n"
        << "  \"forbidden_flags\": " << cfg.forbidden_flags << ",\n"
        << "  \"min_multiplicity\": " << cfg.min_multiplicity << ",\n"
        << "  \"required_trigger_mask\": " << cfg.required_trigger_mask << "\n"
        << "}";
    return oss.str();
}

bool from_json(std::string_view json, TriggerConfig& out_cfg) noexcept {
    if (json.empty()) {
        return false;
    }
    TriggerConfig parsed = out_cfg;
    bool any_found = false;
    any_found |= parse_float_field(json, "min_energy_mev", parsed.min_energy_mev);
    any_found |= parse_float_field(json, "max_energy_mev", parsed.max_energy_mev);
    any_found |= parse_float_field(json, "max_abs_timing_ps", parsed.max_abs_timing_ps);
    any_found |= parse_float_field(json, "max_pileup_score", parsed.max_pileup_score);
    any_found |= parse_uint_field(json, "required_flags", parsed.required_flags);
    any_found |= parse_uint_field(json, "forbidden_flags", parsed.forbidden_flags);
    any_found |= parse_uint_field(json, "min_multiplicity", parsed.min_multiplicity);
    any_found |= parse_uint_field(json, "required_trigger_mask", parsed.required_trigger_mask);
    if (any_found) {
        out_cfg = parsed;
    }
    return any_found;
}

} // namespace chronoring
