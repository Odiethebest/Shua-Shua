#pragma once
//
// bench/common.hpp — shared harness for the Shua Shua benchmarks.
//
// This is BENCH SCAFFOLDING, not engine code. It contains no ranking logic; it
// only times things the engine already does.
//
// Measurement discipline used everywhere in bench/:
//
//   * MIN of rounds, not mean. Noise on a laptop only ever ADDS time, so the
//     minimum over many rounds is the closest thing to "what the machine can
//     actually do". Means drift with background load; mins are reproducible.
//   * INTERLEAVED variants. When comparing A vs B we alternate them round by
//     round, so a thermal ramp or a scheduler hiccup hits both equally instead
//     of penalising whichever ran second.
//   * A volatile sink. Every timed lambda returns a double that is accumulated
//     and finally written to a volatile, so -O2 cannot delete the work.
//
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

namespace bench {

using clk = std::chrono::steady_clock;

inline double us(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

// Is the hand-written NEON kernel actually compiled in on this target?
// dot.hpp guards the intrinsics with `#if defined(__ARM_NEON)`; everywhere else
// dot_simd is literally `return dot_scalar(...)`.
inline bool neon_active() {
#if defined(__ARM_NEON)
    return true;
#else
    return false;
#endif
}

inline const char* kernel_backend() {
    return neon_active() ? "NEON (arm64 intrinsics)" : "scalar fallback (no __ARM_NEON)";
}

// Accumulates the value returned by each timed lambda so the optimiser keeps it.
inline double g_sink = 0.0;

// Run `f` `iters` times per round for `rounds` rounds; return the best
// (minimum) per-iteration time in microseconds.
template <typename F>
double best_us(int rounds, int iters, F&& f) {
    double best = std::numeric_limits<double>::infinity();
    for (int r = 0; r < rounds; ++r) {
        const auto s = clk::now();
        for (int i = 0; i < iters; ++i) g_sink += f();
        const auto e = clk::now();
        best = std::min(best, us(s, e) / iters);
    }
    return best;
}

// Interleave N variants round by round and return each one's best time.
// `run(v)` executes variant v once and returns a value to accumulate.
template <typename F>
std::vector<double> best_us_interleaved(int variants, int rounds, int iters, F&& run) {
    std::vector<double> best(static_cast<std::size_t>(variants),
                             std::numeric_limits<double>::infinity());
    for (int r = 0; r < rounds; ++r) {
        for (int v = 0; v < variants; ++v) {
            const auto s = clk::now();
            for (int i = 0; i < iters; ++i) g_sink += run(v);
            const auto e = clk::now();
            best[static_cast<std::size_t>(v)] =
                std::min(best[static_cast<std::size_t>(v)], us(s, e) / iters);
        }
    }
    return best;
}

inline void keep_sink() {
    volatile double keep = g_sink;
    (void)keep;
}

// ---- output formatting (kept plain so RESULTS.md is a verbatim paste) -------

inline void title(const char* s) {
    std::printf("\n=== %s ===\n", s);
}

inline void note(const char* s) {
    std::printf("  note: %s\n", s);
}

inline void row(const char* label, double value_us) {
    std::printf("  %-46s %9.2f us\n", label, value_us);
}

inline void row_x(const char* label, double factor) {
    std::printf("  %-46s %9.2fx\n", label, factor);
}

inline void row_pct(const char* label, double pct) {
    std::printf("  %-46s %8.1f%%\n", label, pct);
}

inline void harness(int rounds, int iters) {
    std::printf("  harness: min of %d rounds x %d iters, variants interleaved\n", rounds, iters);
}

}  // namespace bench
