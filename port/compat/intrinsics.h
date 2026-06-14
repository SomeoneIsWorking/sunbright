#ifndef SMSPORT_COMPAT_INTRINSICS_H_
#define SMSPORT_COMPAT_INTRINSICS_H_

// ===========================================================================
// PPC intrinsic shim for the native x86-64 port of Super Mario Sunshine.
//
// The decomp targets the MetroWerks CodeWarrior PowerPC compiler, which
// exposes a handful of Gekko/Broadway hardware operations as compiler
// intrinsics (the `__name` forms below). g++/clang on x86-64 do not provide
// them, so we supply host equivalents here. This header is FORCE-INCLUDED
// (`-include compat/intrinsics.h`) into every translation unit so the
// intrinsics are visible before any decomp source references them.
//
// Bit-accuracy note: __frsqrte / __fres are NOT 1/sqrt(x) and 1/x. The Gekko
// FPU computes them via a piecewise lookup-table approximation (~12-bit
// mantissa). MSL's sqrt()/sqrtf() and the game's own math (MathUtil,
// JPAMath) refine that estimate with Newton-Raphson, so the *exact estimate
// bits* matter — a full-precision divide lands on a different refined result.
// The implementations below are ported verbatim from Dolphin's
// Common::ApproximateReciprocal[SquareRoot] (Source/Core/Common/FloatUtils.cpp),
// which is the same bit-accurate hardware-table math that runtime/intrinsics.h
// reuses in the recompiler build. Self-contained here (no Dolphin link).
// ===========================================================================

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace smsport_compat {

// C++17-portable bit_cast (std::bit_cast is C++20). Same-size trivial copy.
template <typename To, typename From>
inline To bit_cast_(const From& src)
{
    static_assert(sizeof(To) == sizeof(From), "bit_cast size mismatch");
    To dst;
    std::memcpy(&dst, &src, sizeof(To));
    return dst;
}

struct BaseAndDec {
    int m_base;
    int m_dec;
};

static constexpr std::uint64_t kDoubleQBit = 0x0008000000000000ULL;

inline double make_quiet(double d)
{
    const std::uint64_t integral = bit_cast_<std::uint64_t>(d) | kDoubleQBit;
    return bit_cast_<double>(integral);
}

// Reciprocal-square-root estimate table (frsqrte) — Dolphin FloatUtils.cpp.
static constexpr BaseAndDec kFrsqrteExpected[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
    {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
    {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
    {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
    {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
    {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
    {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};

inline double approximate_reciprocal_sqrt(double val)
{
    std::int64_t integral = bit_cast_<std::int64_t>(val);
    std::int64_t mantissa = integral & ((1LL << 52) - 1);
    const std::int64_t sign = integral & (1ULL << 63);
    std::int64_t exponent = integral & (0x7FFLL << 52);

    if (mantissa == 0 && exponent == 0)
        return sign ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();

    if (exponent == (0x7FFLL << 52)) {
        if (mantissa == 0) {
            if (sign)
                return std::numeric_limits<double>::quiet_NaN();
            return 0.0;
        }
        return make_quiet(val);
    }

    if (sign)
        return std::numeric_limits<double>::quiet_NaN();

    if (!exponent) {
        do {
            exponent -= 1LL << 52;
            mantissa <<= 1;
        } while (!(mantissa & (1LL << 52)));
        mantissa &= (1LL << 52) - 1;
        exponent += 1LL << 52;
    }

    const std::int64_t exponent_lsb = exponent & (1LL << 52);
    exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
    integral = sign | exponent;

    const int i = static_cast<int>((exponent_lsb | mantissa) >> 37);
    const BaseAndDec& entry = kFrsqrteExpected[i / 2048];
    integral |= static_cast<std::int64_t>(entry.m_base + entry.m_dec * (i % 2048)) << 26;

    return bit_cast_<double>(integral);
}

// Reciprocal estimate table (fres) — Dolphin FloatUtils.cpp.
static constexpr BaseAndDec kFresExpected[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313},
    {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261},
    {0x430800, 0x245}, {0x3e8000, 0x22a}, {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5},
    {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
    {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b}, {0x124400, 0x143},
    {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};

inline double approximate_reciprocal(double val)
{
    std::int64_t integral = bit_cast_<std::int64_t>(val);
    const std::int64_t mantissa = integral & ((1LL << 52) - 1);
    const std::int64_t sign = integral & (1ULL << 63);
    std::int64_t exponent = integral & (0x7FFLL << 52);

    if (mantissa == 0 && exponent == 0)
        return std::copysign(std::numeric_limits<double>::infinity(), val);

    if (exponent == (0x7FFLL << 52)) {
        if (mantissa == 0)
            return std::copysign(0.0, val);
        return make_quiet(val);
    }

    if (exponent < (895LL << 52))
        return std::copysign(std::numeric_limits<float>::max(), val);

    if (exponent >= (1149LL << 52))
        return std::copysign(0.0, val);

    exponent = (0x7FDLL << 52) - exponent;

    const int i = static_cast<int>(mantissa >> 37);
    const BaseAndDec& entry = kFresExpected[i / 1024];
    integral = sign | exponent;
    integral |= static_cast<std::int64_t>(entry.m_base - (entry.m_dec * (i % 1024) + 1) / 2) << 29;

    return bit_cast_<double>(integral);
}

} // namespace smsport_compat

// --- CodeWarrior PPC intrinsics ------------------------------------------

// Reciprocal-sqrt / reciprocal estimates (bit-accurate Gekko table).
inline double __frsqrte(double v) { return smsport_compat::approximate_reciprocal_sqrt(v); }
inline float  __fres(float v)     { return (float)smsport_compat::approximate_reciprocal((double)v); }

// Floating-point absolute value (CodeWarrior fabs/fabsf builtins). The Gekko
// `fabs` clears the sign bit; std::fabs is the exact equivalent.
inline double __fabs(double v)  { return std::fabs(v); }
inline float  __fabsf(float v)  { return std::fabs(v); }

// Count leading zeros (cntlzw): PPC defines clz(0) == 32.
inline int __cntlzw(unsigned int v) { return v ? __builtin_clz(v) : 32; }

// Floating select (fsel): result = (a >= 0) ? b : c. Branchless on Gekko.
inline double __fsel(double a, double b, double c) { return (a >= 0.0) ? b : c; }
inline float  __fself(float a, float b, float c)   { return (a >= 0.0f) ? b : c; }

// Floating negative-absolute (fnabs): set the sign bit.
inline double __fnabs(double v) { return -std::fabs(v); }

#endif // SMSPORT_COMPAT_INTRINSICS_H_
