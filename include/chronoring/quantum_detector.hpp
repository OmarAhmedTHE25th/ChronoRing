#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace chronoring {

enum class CryoDetectorKind : std::uint16_t {
    SNSPD        = 0, // Superconducting Nanowire Single-Photon Detector
    TES          = 1, // Transition-Edge Sensor microcalorimeter
    MKID         = 2, // Microwave Kinetic Inductance Detector
    SQUID        = 3, // Superconducting Quantum Interference Device magnetometer
    IonTrapQubit = 4  // Trapped-ion fluorescence readout
};

enum CryoHealthFlags : std::uint32_t {
    kCryoOk             = 0u,
    kCryoQuenchDetected = 1u << 0,
    kCryoFluxJump       = 1u << 1,
    kCryoTempDrift      = 1u << 2,
    kCryoSaturated      = 1u << 3
};

struct QuantumEventSample {
    std::uint64_t timestamp_ns{0};
    std::uint64_t sequence_id{0};
    std::uint32_t detector_id{0};
    std::uint32_t pixel_id{0};
    float arrival_ps{0.0f};
    float pulse_integral{0.0f};
    float iq_i{0.0f};
    float iq_q{0.0f};
    float bath_temp_mk{100.0f};
    float bias_current_ua{15.0f};
    std::uint32_t health_flags{kCryoOk};
    std::uint16_t coincidence_count{1};
    CryoDetectorKind kind{CryoDetectorKind::SNSPD};
};

struct QuantumCalibration {
    float gain_mev_per_unit{1.0f};
    float offset_mev{0.0f};
    float time_walk_ps_per_unit{0.0f};
    float nominal_bath_temp_mk{100.0f};
    float max_temp_drift_mk{10.0f};
    float max_bias_ua{100.0f};
    CryoDetectorKind detector_kind{CryoDetectorKind::SNSPD};
};

[[nodiscard]] inline EventFrame encode_quantum_sample(
    const QuantumEventSample& s,
    const QuantumCalibration& cal) noexcept {
    EventFrame f{};
    f.timestamp_ns = s.timestamp_ns;
    f.sequence_id = s.sequence_id;
    f.detector_id = s.detector_id;
    f.channel_id = s.pixel_id;

    // MKID readout encodes pulse amplitude in complex IQ plane; TES/SNSPD use pulse integral.
    const float raw_amp =
        (s.kind == CryoDetectorKind::MKID) ? std::hypot(s.iq_i, s.iq_q) : s.pulse_integral;
    f.energy_mev = raw_amp * cal.gain_mev_per_unit + cal.offset_mev;
    f.timing_ps = s.arrival_ps - cal.time_walk_ps_per_unit * raw_amp;

    // Store normalized IQ vector and complex phase angle in spatial/aux slots (zero overhead).
    f.pos_x_mm = s.iq_i;
    f.pos_y_mm = s.iq_q;
    f.pos_z_mm = std::atan2(s.iq_q, s.iq_i);

    const float temp_delta = std::fabs(s.bath_temp_mk - cal.nominal_bath_temp_mk);
    const float temp_ratio =
        (cal.max_temp_drift_mk > 0.0f) ? (temp_delta / cal.max_temp_drift_mk) : 0.0f;
    f.pileup_score = (s.bias_current_ua > cal.max_bias_ua)
                         ? 1.0f
                         : std::min(1.0f, temp_ratio * 0.25f);

    std::uint32_t qflags = kFlagValid | kFlagCalibrated;
    if ((s.health_flags & (kCryoQuenchDetected | kCryoFluxJump)) != 0u) {
        qflags |= kFlagVeto;
    }
    if ((s.health_flags & kCryoSaturated) != 0u) {
        qflags |= kFlagSaturated;
    }
    if ((s.health_flags & kCryoTempDrift) != 0u || temp_delta > cal.max_temp_drift_mk) {
        qflags |= kFlagPileupWarn;
    }

    f.quality_flags = qflags;
    f.multiplicity = s.coincidence_count;
    f.trigger_mask = static_cast<std::uint16_t>(1u << static_cast<unsigned>(s.kind));
    return f;
}

class QuantumDetectorSource {
public:
    explicit QuantumDetectorSource(QuantumCalibration cal = {}) noexcept
        : calibration_(cal) {}

    [[nodiscard]] const QuantumCalibration& calibration() const noexcept { return calibration_; }
    void set_calibration(const QuantumCalibration& cal) noexcept { calibration_ = cal; }

    [[nodiscard]] EventFrame encode(const QuantumEventSample& sample) const noexcept {
        return encode_quantum_sample(sample, calibration_);
    }

    template <typename Target>
    [[nodiscard]] bool ingest_one(const QuantumEventSample& sample, Target& target) const noexcept {
        const EventFrame frame = encode(sample);
        if constexpr (requires(Target& t, const EventFrame& ev) { t.submit(ev); }) {
            return target.submit(frame);
        } else {
            return target.try_push(frame);
        }
    }

    template <typename Target>
    [[nodiscard]] std::size_t ingest_batch(std::span<const QuantumEventSample> samples,
                                           Target& target) const noexcept {
        std::size_t pushed = 0;
        for (const QuantumEventSample& s : samples) {
            if (ingest_one(s, target)) {
                ++pushed;
            }
        }
        return pushed;
    }

private:
    QuantumCalibration calibration_{};
};

// Trigger configuration presets tailored to cryogenic quantum sensors.
[[nodiscard]] inline TriggerConfig snspd_coincidence_trigger(
    std::uint16_t min_coincidence = 2,
    float max_timing_ps = 120.0f) noexcept {
    TriggerConfig cfg{};
    cfg.min_energy_mev = 0.1f;
    cfg.max_energy_mev = 5000.0f;
    cfg.max_abs_timing_ps = max_timing_ps;
    cfg.max_pileup_score = 0.5f;
    cfg.required_flags = kFlagValid | kFlagCalibrated;
    cfg.forbidden_flags = kFlagSaturated | kFlagVeto;
    cfg.min_multiplicity = min_coincidence;
    cfg.required_trigger_mask =
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(CryoDetectorKind::SNSPD));
    return cfg;
}

[[nodiscard]] inline TriggerConfig tes_calorimeter_trigger(
    float min_energy_mev = 1.0f,
    float max_energy_mev = 2000.0f) noexcept {
    TriggerConfig cfg{};
    cfg.min_energy_mev = min_energy_mev;
    cfg.max_energy_mev = max_energy_mev;
    cfg.max_abs_timing_ps = 1000.0f;
    cfg.max_pileup_score = 0.4f;
    cfg.required_flags = kFlagValid | kFlagCalibrated;
    cfg.forbidden_flags = kFlagSaturated | kFlagVeto;
    cfg.min_multiplicity = 1;
    cfg.required_trigger_mask =
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(CryoDetectorKind::TES));
    return cfg;
}

[[nodiscard]] inline TriggerConfig mkid_photon_trigger(
    float min_energy_mev = 0.5f,
    float max_energy_mev = 1000.0f) noexcept {
    TriggerConfig cfg{};
    cfg.min_energy_mev = min_energy_mev;
    cfg.max_energy_mev = max_energy_mev;
    cfg.max_abs_timing_ps = 500.0f;
    cfg.max_pileup_score = 0.4f;
    cfg.required_flags = kFlagValid | kFlagCalibrated;
    cfg.forbidden_flags = kFlagSaturated | kFlagVeto;
    cfg.min_multiplicity = 1;
    cfg.required_trigger_mask =
        static_cast<std::uint16_t>(1u << static_cast<unsigned>(CryoDetectorKind::MKID));
    return cfg;
}

} // namespace chronoring
