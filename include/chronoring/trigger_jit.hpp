#pragma once

#include "chronoring/event_frame.hpp"
#include "chronoring/simd_trigger.hpp"
#include "chronoring/status.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace chronoring {

enum class TriggerOp : std::uint8_t {
    CmpGeF32 = 0,
    CmpLeF32,
    CmpAbsLeF32,
    CmpGtF32,
    CmpLtF32,
    BitAndEqU32,
    BitAndZeroU32,
    BitAndNonZeroU32,
    CmpGeU16,
    CmpEqU32,
    CmpNeU32,
    And,
    Or,
    Not
};

enum class TriggerField : std::uint8_t {
    Energy = 0,
    Timing,
    Pileup,
    PosX,
    PosY,
    PosZ,
    Flags,
    DetectorId,
    ChannelId,
    Multiplicity,
    TriggerMask
};

struct TriggerInstr {
    TriggerOp op{TriggerOp::And};
    TriggerField field{TriggerField::Energy};
    std::uint8_t dst_reg{0};
    std::uint8_t src_reg_a{0};
    std::uint8_t src_reg_b{0};
    std::uint8_t pad[3]{};
    float imm_f32{0.0f};
    std::uint32_t imm_u32{0};
    std::uint32_t imm_u32_aux{0};
};

// Move-only RAII holder for an optional native RW->RX JIT-compiled code page.
class NativeJitBlob {
public:
    NativeJitBlob() noexcept = default;
    ~NativeJitBlob();

    NativeJitBlob(const NativeJitBlob&) = delete;
    NativeJitBlob& operator=(const NativeJitBlob&) = delete;
    NativeJitBlob(NativeJitBlob&& other) noexcept;
    NativeJitBlob& operator=(NativeJitBlob&& other) noexcept;

    [[nodiscard]] bool allocate_and_seal(const std::uint8_t* code, std::size_t size) noexcept;
    void release() noexcept;

    [[nodiscard]] const void* code_ptr() const noexcept { return mem_; }
    [[nodiscard]] std::size_t code_size() const noexcept { return size_; }

private:
    void* mem_{nullptr};
    std::size_t size_{0};
};

struct TriggerProgram {
    std::vector<TriggerInstr> instructions{};
    std::uint8_t result_reg{0};
    SimdBackend compiled_backend{SimdBackend::Scalar};
    NativeJitBlob native_jit{};
    std::string source_dsl{};
};

// Parses and compiles a declarative trigger DSL expression into register bytecode
// and dispatches evaluation onto the best available SIMD vector engine.
//
// Supported syntax examples:
//   "energy >= 10.0 && abs(timing) <= 120.0 && (flags & 1) == 1 && (flags & 12) == 0"
//   "(detector_id == 0 || detector_id == 2) && !(pileup > 0.35) && multiplicity >= 2"
[[nodiscard]] Expected<TriggerProgram, RingStatus> compile_trigger_dsl(
    std::string_view expr,
    SimdBackend requested = SimdBackend::Auto);

[[nodiscard]] std::size_t evaluate_trigger_program_scalar(
    const TriggerProgram& prog,
    const ColumnarPage& page,
    std::uint64_t* out_bitmask_words = nullptr) noexcept;

[[nodiscard]] std::size_t evaluate_trigger_program(
    const TriggerProgram& prog,
    const ColumnarPage& page,
    std::uint64_t* out_bitmask_words = nullptr) noexcept;

} // namespace chronoring
