#include "chronoring/trigger_jit.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

#if defined(__linux__) && !defined(_WIN32)
#include <sys/mman.h>
#endif

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <immintrin.h>
#define CHRONORING_JIT_X86 1
#define CHRONORING_JIT_TARGET_AVX2 __attribute__((target("avx2")))
#define CHRONORING_JIT_TARGET_AVX512 __attribute__((target("avx512f,avx512vl,avx512bw")))
#else
#define CHRONORING_JIT_TARGET_AVX2
#define CHRONORING_JIT_TARGET_AVX512
#endif

namespace chronoring {

namespace {

constexpr std::size_t kMaxDslRegs = 64;

enum class TokKind {
    Ident,
    Number,
    LParen,
    RParen,
    AndAnd,
    OrOr,
    Bang,
    Amp,
    EqEq,
    BangEq,
    Ge,
    Le,
    Gt,
    Lt,
    End,
    Invalid
};

struct Token {
    TokKind kind{TokKind::Invalid};
    std::string_view text{};
    double fval{0.0};
    std::uint64_t uval{0};
    bool is_float{false};
};

std::vector<Token> tokenize(std::string_view expr) {
    std::vector<Token> toks;
    std::size_t i = 0;
    while (i < expr.size()) {
        if (std::isspace(static_cast<unsigned char>(expr[i]))) {
            ++i;
            continue;
        }
        if (expr[i] == '(') {
            toks.push_back(Token{TokKind::LParen, expr.substr(i, 1)});
            ++i;
        } else if (expr[i] == ')') {
            toks.push_back(Token{TokKind::RParen, expr.substr(i, 1)});
            ++i;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == "&&") {
            toks.push_back(Token{TokKind::AndAnd, expr.substr(i, 2)});
            i += 2;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == "||") {
            toks.push_back(Token{TokKind::OrOr, expr.substr(i, 2)});
            i += 2;
        } else if (expr[i] == '&') {
            toks.push_back(Token{TokKind::Amp, expr.substr(i, 1)});
            ++i;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == "==") {
            toks.push_back(Token{TokKind::EqEq, expr.substr(i, 2)});
            i += 2;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == "!=") {
            toks.push_back(Token{TokKind::BangEq, expr.substr(i, 2)});
            i += 2;
        } else if (expr[i] == '!') {
            toks.push_back(Token{TokKind::Bang, expr.substr(i, 1)});
            ++i;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == ">=") {
            toks.push_back(Token{TokKind::Ge, expr.substr(i, 2)});
            i += 2;
        } else if (i + 1 < expr.size() && expr.substr(i, 2) == "<=") {
            toks.push_back(Token{TokKind::Le, expr.substr(i, 2)});
            i += 2;
        } else if (expr[i] == '>') {
            toks.push_back(Token{TokKind::Gt, expr.substr(i, 1)});
            ++i;
        } else if (expr[i] == '<') {
            toks.push_back(Token{TokKind::Lt, expr.substr(i, 1)});
            ++i;
        } else if (std::isalpha(static_cast<unsigned char>(expr[i])) || expr[i] == '_') {
            const std::size_t start = i;
            while (i < expr.size() &&
                   (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_')) {
                ++i;
            }
            toks.push_back(Token{TokKind::Ident, expr.substr(start, i - start)});
        } else if (std::isdigit(static_cast<unsigned char>(expr[i])) ||
                   (expr[i] == '-' && i + 1 < expr.size() &&
                    std::isdigit(static_cast<unsigned char>(expr[i + 1])))) {
            const std::size_t start = i;
            bool is_hex = (i + 2 < expr.size() && expr[i] == '0' &&
                           (expr[i + 1] == 'x' || expr[i + 1] == 'X'));
            bool is_float = false;
            if (is_hex) {
                i += 2;
                while (i < expr.size() && std::isxdigit(static_cast<unsigned char>(expr[i]))) {
                    ++i;
                }
            } else {
                if (expr[i] == '-') {
                    ++i;
                    is_float = true;
                }
                while (i < expr.size() &&
                       (std::isdigit(static_cast<unsigned char>(expr[i])) || expr[i] == '.')) {
                    if (expr[i] == '.') {
                        is_float = true;
                    }
                    ++i;
                }
            }
            const std::string_view sv = expr.substr(start, i - start);
            std::string tmp(sv);
            Token tok{TokKind::Number, sv};
            tok.is_float = is_float;
            if (is_hex) {
                tok.uval = std::strtoull(tmp.c_str(), nullptr, 16);
                tok.fval = static_cast<double>(tok.uval);
            } else if (is_float) {
                tok.fval = std::strtod(tmp.c_str(), nullptr);
                tok.uval = (tok.fval >= 0.0) ? static_cast<std::uint64_t>(tok.fval) : 0ULL;
            } else {
                tok.uval = std::strtoull(tmp.c_str(), nullptr, 10);
                tok.fval = static_cast<double>(tok.uval);
            }
            toks.push_back(tok);
        } else {
            toks.push_back(Token{TokKind::Invalid, expr.substr(i, 1)});
            return toks;
        }
    }
    toks.push_back(Token{TokKind::End, {}});
    return toks;
}

bool parse_field(std::string_view name, TriggerField& out, bool& is_float_field) noexcept {
    if (name == "energy" || name == "energy_mev") {
        out = TriggerField::Energy;
        is_float_field = true;
        return true;
    }
    if (name == "timing" || name == "timing_ps") {
        out = TriggerField::Timing;
        is_float_field = true;
        return true;
    }
    if (name == "pileup" || name == "pileup_score") {
        out = TriggerField::Pileup;
        is_float_field = true;
        return true;
    }
    if (name == "pos_x" || name == "pos_x_mm") {
        out = TriggerField::PosX;
        is_float_field = true;
        return true;
    }
    if (name == "pos_y" || name == "pos_y_mm") {
        out = TriggerField::PosY;
        is_float_field = true;
        return true;
    }
    if (name == "pos_z" || name == "pos_z_mm") {
        out = TriggerField::PosZ;
        is_float_field = true;
        return true;
    }
    if (name == "flags" || name == "quality_flags") {
        out = TriggerField::Flags;
        is_float_field = false;
        return true;
    }
    if (name == "detector_id") {
        out = TriggerField::DetectorId;
        is_float_field = false;
        return true;
    }
    if (name == "channel_id") {
        out = TriggerField::ChannelId;
        is_float_field = false;
        return true;
    }
    if (name == "multiplicity") {
        out = TriggerField::Multiplicity;
        is_float_field = false;
        return true;
    }
    if (name == "trigger_mask") {
        out = TriggerField::TriggerMask;
        is_float_field = false;
        return true;
    }
    return false;
}

class DslParser {
public:
    explicit DslParser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    Expected<std::uint8_t, RingStatus> parse_program(std::vector<TriggerInstr>& out) {
        if (toks_.empty() || peek().kind == TokKind::End || peek().kind == TokKind::Invalid) {
            return unexpected(RingStatus::InvalidArgument);
        }
        auto reg = parse_or(out);
        if (!reg.has_value() || peek().kind != TokKind::End) {
            return unexpected(RingStatus::InvalidArgument);
        }
        return reg;
    }

private:
    const Token& peek(std::size_t look = 0) const noexcept {
        if (pos_ + look >= toks_.size()) {
            return toks_.back();
        }
        return toks_[pos_ + look];
    }

    const Token& advance() noexcept {
        const Token& cur = peek();
        if (pos_ + 1 < toks_.size()) {
            ++pos_;
        }
        return cur;
    }

    Expected<std::uint8_t, RingStatus> alloc_reg() noexcept {
        if (next_reg_ >= kMaxDslRegs) {
            return unexpected(RingStatus::InvalidArgument);
        }
        return next_reg_++;
    }

    Expected<std::uint8_t, RingStatus> parse_or(std::vector<TriggerInstr>& out) {
        auto lhs = parse_and(out);
        if (!lhs.has_value()) {
            return lhs;
        }
        while (peek().kind == TokKind::OrOr) {
            advance();
            auto rhs = parse_and(out);
            if (!rhs.has_value()) {
                return rhs;
            }
            auto dst = alloc_reg();
            if (!dst.has_value()) {
                return dst;
            }
            TriggerInstr ins{};
            ins.op = TriggerOp::Or;
            ins.dst_reg = *dst;
            ins.src_reg_a = *lhs;
            ins.src_reg_b = *rhs;
            out.push_back(ins);
            lhs = *dst;
        }
        return lhs;
    }

    Expected<std::uint8_t, RingStatus> parse_and(std::vector<TriggerInstr>& out) {
        auto lhs = parse_unary(out);
        if (!lhs.has_value()) {
            return lhs;
        }
        while (peek().kind == TokKind::AndAnd) {
            advance();
            auto rhs = parse_unary(out);
            if (!rhs.has_value()) {
                return rhs;
            }
            auto dst = alloc_reg();
            if (!dst.has_value()) {
                return dst;
            }
            TriggerInstr ins{};
            ins.op = TriggerOp::And;
            ins.dst_reg = *dst;
            ins.src_reg_a = *lhs;
            ins.src_reg_b = *rhs;
            out.push_back(ins);
            lhs = *dst;
        }
        return lhs;
    }

    Expected<std::uint8_t, RingStatus> parse_unary(std::vector<TriggerInstr>& out) {
        if (peek().kind == TokKind::Bang) {
            advance();
            auto inner = parse_unary(out);
            if (!inner.has_value()) {
                return inner;
            }
            auto dst = alloc_reg();
            if (!dst.has_value()) {
                return dst;
            }
            TriggerInstr ins{};
            ins.op = TriggerOp::Not;
            ins.dst_reg = *dst;
            ins.src_reg_a = *inner;
            out.push_back(ins);
            return *dst;
        }

        // Distinguish `((flags & mask) == val)` vs grouped boolean `(expr)`
        if (peek().kind == TokKind::LParen) {
            if (peek(1).kind == TokKind::Ident && peek(2).kind == TokKind::Amp) {
                return parse_bitmask_predicate(out, true);
            }
            advance();
            auto inner = parse_or(out);
            if (!inner.has_value() || peek().kind != TokKind::RParen) {
                return unexpected(RingStatus::InvalidArgument);
            }
            advance();
            return inner;
        }

        return parse_predicate(out);
    }

    Expected<std::uint8_t, RingStatus> parse_bitmask_predicate(std::vector<TriggerInstr>& out,
                                                               bool wrapped_parens) {
        if (wrapped_parens) {
            advance(); // '('
        }
        const Token field_tok = advance();
        TriggerField fld{};
        bool is_float = false;
        if (field_tok.kind != TokKind::Ident ||
            !parse_field(field_tok.text, fld, is_float) || is_float) {
            return unexpected(RingStatus::InvalidArgument);
        }
        if (advance().kind != TokKind::Amp) {
            return unexpected(RingStatus::InvalidArgument);
        }
        const Token mask_tok = advance();
        if (mask_tok.kind != TokKind::Number) {
            return unexpected(RingStatus::InvalidArgument);
        }
        if (wrapped_parens && advance().kind != TokKind::RParen) {
            return unexpected(RingStatus::InvalidArgument);
        }
        const TokKind eq_op = advance().kind;
        if (eq_op != TokKind::EqEq && eq_op != TokKind::BangEq) {
            return unexpected(RingStatus::InvalidArgument);
        }
        const Token val_tok = advance();
        if (val_tok.kind != TokKind::Number) {
            return unexpected(RingStatus::InvalidArgument);
        }

        auto dst = alloc_reg();
        if (!dst.has_value()) {
            return dst;
        }
        TriggerInstr ins{};
        ins.field = fld;
        ins.dst_reg = *dst;
        ins.imm_u32 = static_cast<std::uint32_t>(mask_tok.uval);
        ins.imm_u32_aux = static_cast<std::uint32_t>(val_tok.uval);

        if (eq_op == TokKind::EqEq) {
            ins.op = (ins.imm_u32_aux == 0u) ? TriggerOp::BitAndZeroU32 : TriggerOp::BitAndEqU32;
        } else {
            if (ins.imm_u32_aux == 0u) {
                ins.op = TriggerOp::BitAndNonZeroU32;
            } else {
                // `(f & mask) != val` -> Eq then Not
                ins.op = TriggerOp::BitAndEqU32;
                out.push_back(ins);
                auto negated = alloc_reg();
                if (!negated.has_value()) {
                    return negated;
                }
                TriggerInstr nins{};
                nins.op = TriggerOp::Not;
                nins.dst_reg = *negated;
                nins.src_reg_a = *dst;
                out.push_back(nins);
                return *negated;
            }
        }
        out.push_back(ins);
        return *dst;
    }

    Expected<std::uint8_t, RingStatus> parse_predicate(std::vector<TriggerInstr>& out) {
        if (peek().kind != TokKind::Ident) {
            return unexpected(RingStatus::InvalidArgument);
        }
        if (peek().text == "abs") {
            advance();
            if (advance().kind != TokKind::LParen) {
                return unexpected(RingStatus::InvalidArgument);
            }
            const Token ft = advance();
            TriggerField fld{};
            bool is_float = false;
            if (ft.kind != TokKind::Ident || !parse_field(ft.text, fld, is_float) || !is_float) {
                return unexpected(RingStatus::InvalidArgument);
            }
            if (advance().kind != TokKind::RParen) {
                return unexpected(RingStatus::InvalidArgument);
            }
            const TokKind cmp = advance().kind;
            const Token num = advance();
            if (num.kind != TokKind::Number) {
                return unexpected(RingStatus::InvalidArgument);
            }
            if (cmp != TokKind::Le && cmp != TokKind::Lt &&
                cmp != TokKind::Gt && cmp != TokKind::Ge) {
                return unexpected(RingStatus::InvalidArgument);
            }
            auto dst = alloc_reg();
            if (!dst.has_value()) {
                return dst;
            }
            TriggerInstr ins{};
            ins.op = TriggerOp::CmpAbsLeF32;
            ins.field = fld;
            ins.dst_reg = *dst;
            ins.imm_f32 = static_cast<float>(num.fval);
            if (cmp == TokKind::Gt || cmp == TokKind::Ge) {
                // !(abs(x) <= v) for '>' (and exact inverse approximation for float)
                out.push_back(ins);
                auto neg = alloc_reg();
                if (!neg.has_value()) {
                    return neg;
                }
                TriggerInstr nins{};
                nins.op = TriggerOp::Not;
                nins.dst_reg = *neg;
                nins.src_reg_a = *dst;
                out.push_back(nins);
                return *neg;
            }
            out.push_back(ins);
            return *dst;
        }

        if (peek(1).kind == TokKind::Amp) {
            return parse_bitmask_predicate(out, false);
        }

        const Token ft = advance();
        TriggerField fld{};
        bool is_float = false;
        if (!parse_field(ft.text, fld, is_float)) {
            return unexpected(RingStatus::InvalidArgument);
        }
        const TokKind cmp = advance().kind;
        const Token num = advance();
        if (num.kind != TokKind::Number) {
            return unexpected(RingStatus::InvalidArgument);
        }

        auto dst = alloc_reg();
        if (!dst.has_value()) {
            return dst;
        }
        TriggerInstr ins{};
        ins.field = fld;
        ins.dst_reg = *dst;

        if (is_float) {
            ins.imm_f32 = static_cast<float>(num.fval);
            switch (cmp) {
            case TokKind::Ge:
                ins.op = TriggerOp::CmpGeF32;
                break;
            case TokKind::Le:
                ins.op = TriggerOp::CmpLeF32;
                break;
            case TokKind::Gt:
                ins.op = TriggerOp::CmpGtF32;
                break;
            case TokKind::Lt:
                ins.op = TriggerOp::CmpLtF32;
                break;
            default:
                return unexpected(RingStatus::InvalidArgument);
            }
            out.push_back(ins);
            return *dst;
        }

        ins.imm_u32 = static_cast<std::uint32_t>(num.uval);
        switch (cmp) {
        case TokKind::Ge:
            ins.op = TriggerOp::CmpGeU16;
            break;
        case TokKind::EqEq:
            ins.op = TriggerOp::CmpEqU32;
            break;
        case TokKind::BangEq:
            ins.op = TriggerOp::CmpNeU32;
            break;
        case TokKind::Lt: {
            // x < k  <=>  !(x >= k)
            ins.op = TriggerOp::CmpGeU16;
            out.push_back(ins);
            auto neg = alloc_reg();
            if (!neg.has_value()) {
                return neg;
            }
            TriggerInstr nins{};
            nins.op = TriggerOp::Not;
            nins.dst_reg = *neg;
            nins.src_reg_a = *dst;
            out.push_back(nins);
            return *neg;
        }
        case TokKind::Gt: {
            // x > k  <=>  x >= (k + 1)
            ins.op = TriggerOp::CmpGeU16;
            ins.imm_u32 = ins.imm_u32 + 1u;
            break;
        }
        case TokKind::Le: {
            // x <= k  <=>  !(x >= k + 1)
            ins.op = TriggerOp::CmpGeU16;
            ins.imm_u32 = ins.imm_u32 + 1u;
            out.push_back(ins);
            auto neg = alloc_reg();
            if (!neg.has_value()) {
                return neg;
            }
            TriggerInstr nins{};
            nins.op = TriggerOp::Not;
            nins.dst_reg = *neg;
            nins.src_reg_a = *dst;
            out.push_back(nins);
            return *neg;
        }
        default:
            return unexpected(RingStatus::InvalidArgument);
        }
        out.push_back(ins);
        return *dst;
    }

    std::vector<Token> toks_;
    std::size_t pos_{0};
    std::uint8_t next_reg_{0};
};

[[nodiscard]] const float* get_f32_col(const ColumnarPage& page, TriggerField f) noexcept {
    switch (f) {
    case TriggerField::Energy:
        return page.energy_mev.data();
    case TriggerField::Timing:
        return page.timing_ps.data();
    case TriggerField::Pileup:
        return page.pileup_score.data();
    case TriggerField::PosX:
        return page.pos_x_mm.data();
    case TriggerField::PosY:
        return page.pos_y_mm.data();
    case TriggerField::PosZ:
        return page.pos_z_mm.data();
    default:
        return nullptr;
    }
}

[[nodiscard]] std::uint32_t read_u32_col_elem(const ColumnarPage& page,
                                              TriggerField f,
                                              std::size_t idx) noexcept {
    switch (f) {
    case TriggerField::Flags:
        return page.quality_flags[idx];
    case TriggerField::DetectorId:
        return page.detector_id[idx];
    case TriggerField::ChannelId:
        return page.channel_id[idx];
    case TriggerField::Multiplicity:
        return page.multiplicity[idx];
    case TriggerField::TriggerMask:
        return page.trigger_mask[idx];
    default:
        return 0u;
    }
}

[[nodiscard]] bool eval_instr_scalar_one(const TriggerInstr& ins,
                                         const ColumnarPage& page,
                                         std::size_t idx,
                                         const std::array<std::uint8_t, kMaxDslRegs>& regs) noexcept {
    switch (ins.op) {
    case TriggerOp::CmpGeF32:
        return get_f32_col(page, ins.field)[idx] >= ins.imm_f32;
    case TriggerOp::CmpLeF32:
        return get_f32_col(page, ins.field)[idx] <= ins.imm_f32;
    case TriggerOp::CmpAbsLeF32:
        return std::fabs(get_f32_col(page, ins.field)[idx]) <= ins.imm_f32;
    case TriggerOp::CmpGtF32:
        return get_f32_col(page, ins.field)[idx] > ins.imm_f32;
    case TriggerOp::CmpLtF32:
        return get_f32_col(page, ins.field)[idx] < ins.imm_f32;
    case TriggerOp::BitAndEqU32:
        return (read_u32_col_elem(page, ins.field, idx) & ins.imm_u32) == ins.imm_u32_aux;
    case TriggerOp::BitAndZeroU32:
        return (read_u32_col_elem(page, ins.field, idx) & ins.imm_u32) == 0u;
    case TriggerOp::BitAndNonZeroU32:
        return (read_u32_col_elem(page, ins.field, idx) & ins.imm_u32) != 0u;
    case TriggerOp::CmpGeU16:
        return read_u32_col_elem(page, ins.field, idx) >= ins.imm_u32;
    case TriggerOp::CmpEqU32:
        return read_u32_col_elem(page, ins.field, idx) == ins.imm_u32;
    case TriggerOp::CmpNeU32:
        return read_u32_col_elem(page, ins.field, idx) != ins.imm_u32;
    case TriggerOp::And:
        return (regs[ins.src_reg_a] & regs[ins.src_reg_b]) != 0u;
    case TriggerOp::Or:
        return (regs[ins.src_reg_a] | regs[ins.src_reg_b]) != 0u;
    case TriggerOp::Not:
        return regs[ins.src_reg_a] == 0u;
    }
    return false;
}

#ifdef CHRONORING_JIT_X86
CHRONORING_JIT_TARGET_AVX2
std::uint64_t eval_f32_chunk64_avx2(const float* ptr,
                                    TriggerOp op,
                                    float imm,
                                    std::size_t valid_count) noexcept {
    const __m256 vimm = _mm256_set1_ps(imm);
    const __m256 sign_bit = _mm256_set1_ps(-0.0f);
    std::uint64_t word = 0;

    for (std::size_t base = 0; base < valid_count; base += 8) {
        const __m256 v = _mm256_loadu_ps(ptr + base);
        __m256 m{};
        switch (op) {
        case TriggerOp::CmpGeF32:
            m = _mm256_cmp_ps(v, vimm, _CMP_GE_OQ);
            break;
        case TriggerOp::CmpLeF32:
            m = _mm256_cmp_ps(v, vimm, _CMP_LE_OQ);
            break;
        case TriggerOp::CmpAbsLeF32:
            m = _mm256_cmp_ps(_mm256_andnot_ps(sign_bit, v), vimm, _CMP_LE_OQ);
            break;
        case TriggerOp::CmpGtF32:
            m = _mm256_cmp_ps(v, vimm, _CMP_GT_OQ);
            break;
        case TriggerOp::CmpLtF32:
            m = _mm256_cmp_ps(v, vimm, _CMP_LT_OQ);
            break;
        default:
            m = _mm256_setzero_ps();
            break;
        }
        const std::uint64_t bits = static_cast<std::uint64_t>(_mm256_movemask_ps(m));
        word |= (bits << base);
    }
    return word;
}

CHRONORING_JIT_TARGET_AVX512
std::uint64_t eval_f32_chunk64_avx512(const float* ptr,
                                      TriggerOp op,
                                      float imm,
                                      std::size_t valid_count) noexcept {
    const __m512 vimm = _mm512_set1_ps(imm);
    const __m512i abs_mask = _mm512_set1_epi32(0x7FFFFFFF);
    std::uint64_t word = 0;

    for (std::size_t base = 0; base < valid_count; base += 16) {
        const __m512 v = _mm512_loadu_ps(ptr + base);
        __mmask16 m = 0;
        switch (op) {
        case TriggerOp::CmpGeF32:
            m = _mm512_cmp_ps_mask(v, vimm, _CMP_GE_OQ);
            break;
        case TriggerOp::CmpLeF32:
            m = _mm512_cmp_ps_mask(v, vimm, _CMP_LE_OQ);
            break;
        case TriggerOp::CmpAbsLeF32: {
            const __m512 av = _mm512_castsi512_ps(
                _mm512_and_si512(_mm512_castps_si512(v), abs_mask));
            m = _mm512_cmp_ps_mask(av, vimm, _CMP_LE_OQ);
            break;
        }
        case TriggerOp::CmpGtF32:
            m = _mm512_cmp_ps_mask(v, vimm, _CMP_GT_OQ);
            break;
        case TriggerOp::CmpLtF32:
            m = _mm512_cmp_ps_mask(v, vimm, _CMP_LT_OQ);
            break;
        default:
            m = 0;
            break;
        }
        word |= (static_cast<std::uint64_t>(m) << base);
    }
    return word;
}
#endif

} // namespace

// ============================================================================
// NativeJitBlob
// ============================================================================

NativeJitBlob::~NativeJitBlob() {
    release();
}

NativeJitBlob::NativeJitBlob(NativeJitBlob&& other) noexcept
    : mem_(other.mem_), size_(other.size_) {
    other.mem_ = nullptr;
    other.size_ = 0;
}

NativeJitBlob& NativeJitBlob::operator=(NativeJitBlob&& other) noexcept {
    if (this != &other) {
        release();
        mem_ = other.mem_;
        size_ = other.size_;
        other.mem_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

bool NativeJitBlob::allocate_and_seal(const std::uint8_t* code, std::size_t size) noexcept {
    release();
    if (code == nullptr || size == 0) {
        return false;
    }
#if defined(__linux__) && !defined(_WIN32)
    void* buf = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        return false;
    }
    std::memcpy(buf, code, size);
    if (::mprotect(buf, size, PROT_READ | PROT_EXEC) != 0) {
        ::munmap(buf, size);
        return false;
    }
    mem_ = buf;
    size_ = size;
    return true;
#else
    (void)code;
    (void)size;
    return false;
#endif
}

void NativeJitBlob::release() noexcept {
    if (mem_ != nullptr) {
#if defined(__linux__) && !defined(_WIN32)
        ::munmap(mem_, size_);
#endif
        mem_ = nullptr;
        size_ = 0;
    }
}

// ============================================================================
// DSL Compiler & Evaluators
// ============================================================================

Expected<TriggerProgram, RingStatus> compile_trigger_dsl(std::string_view expr,
                                                       SimdBackend requested) {
    std::vector<Token> toks = tokenize(expr);
    DslParser parser(std::move(toks));

    TriggerProgram prog;
    prog.source_dsl = std::string(expr);
    prog.compiled_backend = active_backend(requested);

    auto res_reg = parser.parse_program(prog.instructions);
    if (!res_reg.has_value()) {
        return unexpected(res_reg.error());
    }
    prog.result_reg = *res_reg;

#if defined(__linux__) && defined(__x86_64__)
    // Emit a compact RW->RX native x86-64 epilogue/version probe if mmap+mprotect RX is allowed.
    const std::array<std::uint8_t, 6> ret_stub = {
        0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
        0xC3                          // ret
    };
    (void)prog.native_jit.allocate_and_seal(ret_stub.data(), ret_stub.size());
#endif

    return prog;
}

std::size_t evaluate_trigger_program_scalar(const TriggerProgram& prog,
                                            const ColumnarPage& page,
                                            std::uint64_t* out_bitmask_words) noexcept {
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (out_bitmask_words != nullptr && n > 0) {
        const std::size_t words = (n + 63u) / 64u;
        std::memset(out_bitmask_words, 0, words * sizeof(std::uint64_t));
    }
    if (n == 0 || prog.instructions.empty()) {
        return 0;
    }

    std::size_t passed = 0;
    std::array<std::uint8_t, kMaxDslRegs> regs{};
    for (std::size_t i = 0; i < n; ++i) {
        for (const TriggerInstr& ins : prog.instructions) {
            regs[ins.dst_reg] = eval_instr_scalar_one(ins, page, i, regs) ? 1u : 0u;
        }
        if (regs[prog.result_reg] != 0u) {
            if (out_bitmask_words != nullptr) {
                out_bitmask_words[i >> 6] |= (std::uint64_t{1} << (i & 63u));
            }
            ++passed;
        }
    }
    return passed;
}

std::size_t evaluate_trigger_program(const TriggerProgram& prog,
                                     const ColumnarPage& page,
                                     std::uint64_t* out_bitmask_words) noexcept {
    const std::size_t n =
        std::min<std::size_t>(page.header.event_count, kColumnarPageEvents);
    if (out_bitmask_words != nullptr && n > 0) {
        const std::size_t words = (n + 63u) / 64u;
        std::memset(out_bitmask_words, 0, words * sizeof(std::uint64_t));
    }
    if (n == 0 || prog.instructions.empty()) {
        return 0;
    }

    std::size_t passed = 0;
    std::array<std::uint64_t, kMaxDslRegs> vregs{};

    // Evaluate 64 events per outer step: float predicates use AVX-512 / AVX2 SIMD kernels
    // directly over aligned SoA columns; boolean AST nodes collapse to 1-cycle uint64 ops.
    for (std::size_t base = 0; base < n; base += 64u) {
        const std::size_t chunk = std::min<std::size_t>(64u, n - base);
        const std::uint64_t valid_mask =
            (chunk == 64u) ? ~std::uint64_t{0} : ((std::uint64_t{1} << chunk) - 1u);

        for (const TriggerInstr& ins : prog.instructions) {
            switch (ins.op) {
            case TriggerOp::And:
                vregs[ins.dst_reg] = vregs[ins.src_reg_a] & vregs[ins.src_reg_b];
                break;
            case TriggerOp::Or:
                vregs[ins.dst_reg] = vregs[ins.src_reg_a] | vregs[ins.src_reg_b];
                break;
            case TriggerOp::Not:
                vregs[ins.dst_reg] = (~vregs[ins.src_reg_a]) & valid_mask;
                break;
            case TriggerOp::CmpGeF32:
            case TriggerOp::CmpLeF32:
            case TriggerOp::CmpAbsLeF32:
            case TriggerOp::CmpGtF32:
            case TriggerOp::CmpLtF32: {
                const float* col = get_f32_col(page, ins.field) + base;
                std::uint64_t word = 0;
#ifdef CHRONORING_JIT_X86
                if (prog.compiled_backend == SimdBackend::AVX512 && cpu_supports_avx512() &&
                    (chunk % 16u) == 0u) {
                    word = eval_f32_chunk64_avx512(col, ins.op, ins.imm_f32, chunk);
                } else if ((prog.compiled_backend == SimdBackend::AVX2 ||
                            prog.compiled_backend == SimdBackend::AVX512) &&
                           cpu_supports_avx2() && (chunk % 8u) == 0u) {
                    word = eval_f32_chunk64_avx2(col, ins.op, ins.imm_f32, chunk);
                } else
#endif
                {
                    for (std::size_t k = 0; k < chunk; ++k) {
                        const float v = col[k];
                        bool ok = false;
                        if (ins.op == TriggerOp::CmpGeF32) ok = (v >= ins.imm_f32);
                        else if (ins.op == TriggerOp::CmpLeF32) ok = (v <= ins.imm_f32);
                        else if (ins.op == TriggerOp::CmpAbsLeF32) ok = (std::fabs(v) <= ins.imm_f32);
                        else if (ins.op == TriggerOp::CmpGtF32) ok = (v > ins.imm_f32);
                        else if (ins.op == TriggerOp::CmpLtF32) ok = (v < ins.imm_f32);
                        if (ok) {
                            word |= (std::uint64_t{1} << k);
                        }
                    }
                }
                vregs[ins.dst_reg] = word & valid_mask;
                break;
            }
            case TriggerOp::BitAndEqU32:
            case TriggerOp::BitAndZeroU32:
            case TriggerOp::BitAndNonZeroU32:
            case TriggerOp::CmpGeU16:
            case TriggerOp::CmpEqU32:
            case TriggerOp::CmpNeU32: {
                std::uint64_t word = 0;
                for (std::size_t k = 0; k < chunk; ++k) {
                    const std::uint32_t v = read_u32_col_elem(page, ins.field, base + k);
                    bool ok = false;
                    if (ins.op == TriggerOp::BitAndEqU32) ok = ((v & ins.imm_u32) == ins.imm_u32_aux);
                    else if (ins.op == TriggerOp::BitAndZeroU32) ok = ((v & ins.imm_u32) == 0u);
                    else if (ins.op == TriggerOp::BitAndNonZeroU32) ok = ((v & ins.imm_u32) != 0u);
                    else if (ins.op == TriggerOp::CmpGeU16) ok = (v >= ins.imm_u32);
                    else if (ins.op == TriggerOp::CmpEqU32) ok = (v == ins.imm_u32);
                    else if (ins.op == TriggerOp::CmpNeU32) ok = (v != ins.imm_u32);
                    if (ok) {
                        word |= (std::uint64_t{1} << k);
                    }
                }
                vregs[ins.dst_reg] = word & valid_mask;
                break;
            }
            }
        }

        const std::uint64_t res = vregs[prog.result_reg] & valid_mask;
        if (out_bitmask_words != nullptr) {
            out_bitmask_words[base >> 6] = res;
        }
        passed += static_cast<std::size_t>(std::popcount(res));
    }
    return passed;
}

} // namespace chronoring
