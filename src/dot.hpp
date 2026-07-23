#pragma once

#include <cstddef>

// On arm64 (Apple silicon) NEON is the SIMD instruction set, and its intrinsics
// live in <arm_neon.h>. We include it only when NEON is actually available so
// this file still compiles on other targets — a non-ARM host, or a WASM build
// without SIMD — where dot_simd falls back to the scalar path.
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// -----------------------------------------------------------------------------
// dot_scalar / dot_simd — the inner-product kernels.
//
// This is the ONE hot computation in the whole engine: recall scores every
// candidate by the dot product of the query vector with the item vector. M2's
// entire job is to make this fast with hand-written SIMD while proving the result
// is unchanged. dot_scalar is the permanent reference; dot_simd is the optimized
// path; recall runs whichever it is handed, and the parity check compares them.
// -----------------------------------------------------------------------------

// Plain scalar dot product — the reference. Sums left-to-right, one float at a
// time.
//
// WHY this stays genuinely scalar even at -O2: clang will not auto-vectorize a
// floating-point reduction unless -ffast-math is on, because summing in a
// different order can change the result (float addition is not associative). The
// documented build is plain -O2, so this compiles to scalar adds — exactly the
// honest baseline the SIMD path is measured against.
inline float dot_scalar(const float* a, const float* b, std::size_t dim) {
    float sum = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) {
        sum += a[d] * b[d];
    }
    return sum;
}

// Hand-written NEON dot product.
//
// The idea: a NEON register is 128 bits = 4 x float32 ("4 lanes"), so we walk the
// vectors four elements at a time and keep FOUR running partial sums (one per
// lane) instead of one. At the end we add the four lanes together.
//
// WHY width 4: that is simply how many float32s fit in one 128-bit NEON register
// — the natural, and here the only, width. (The x86 AVX2 equivalent would be
// 8-wide __m256; that path is out of scope on arm64.)
inline float dot_simd(const float* a, const float* b, std::size_t dim) {
#if defined(__ARM_NEON)
    // Four independent lane accumulators, all starting at zero.
    float32x4_t acc = vdupq_n_f32(0.0f);

    // Main loop: stride 4 floats per iteration. The guard `d + 4 <= dim` stops
    // before any partial group, so every load reads a full register of 4 valid
    // floats.
    std::size_t d = 0;
    for (; d + 4 <= dim; d += 4) {
        // vld1q_f32 loads 4 consecutive floats. It permits UNALIGNED addresses,
        // so the SoA buffer needs no special alignment — an item vector can start
        // at any offset. That is what lets the store stay a plain vector<float>.
        const float32x4_t va = vld1q_f32(a + d);
        const float32x4_t vb = vld1q_f32(b + d);

        // Lane-wise multiply-accumulate: acc[i] += va[i] * vb[i] for i in 0..3.
        // This is the vector form of the scalar `sum += a*b`, four lanes at once.
        // vmlaq_f32 is the basic multiply-add primitive — a dot product needs
        // nothing fancier.
        acc = vmlaq_f32(acc, va, vb);
    }

    // Horizontal reduction: collapse the 4 lane sums into one scalar. Written as
    // an explicit sum of the four lanes so it needs no NEON background to read;
    // vaddvq_f32(acc) does the same in a single instruction.
    float sum = vgetq_lane_f32(acc, 0) + vgetq_lane_f32(acc, 1)
              + vgetq_lane_f32(acc, 2) + vgetq_lane_f32(acc, 3);

    // Tail: if dim is not a multiple of 4, the main loop left 1..3 elements
    // unprocessed; finish them with scalar adds. WHY keep this even though
    // DIM = 64 is a multiple of 4 (so the tail runs zero times today): the kernel
    // is then correct for ANY dimension, and a future DIM change cannot silently
    // drop the last few elements.
    for (; d < dim; ++d) {
        sum += a[d] * b[d];
    }
    return sum;
#else
    // No NEON on this target: fall back to the scalar kernel so the code still
    // builds and runs everywhere. Parity then trivially holds and speedup is ~1x.
    return dot_scalar(a, b, dim);
#endif
}
