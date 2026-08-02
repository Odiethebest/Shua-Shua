// bench/bench_parity.cpp — naive vs SIMD recall: NUMERICAL DIFF + speedup.
//
// The check itself is the one the repository owner wrote in src/main.cpp
// (run_recall_diagnostics, main.cpp:66-147); this file lifts it into a standalone,
// CI-runnable target. src/main.cpp keeps its copy as the dev entry point — that is
// deliberate, not duplication to be cleaned up: `./shuashua` must stay a
// one-command "show me everything" driver.
//
// What this target adds over the in-driver version:
//   1. A NON-ZERO EXIT CODE when the diff fails, so CI can gate on it.
//   2. An explicit statement of which kernel backend was compiled in, because on
//      any non-ARM target dot_simd IS dot_scalar (dot.hpp:86-90) and the diff is
//      then TRIVIALLY TRUE. A green check must not imply NEON was exercised.
//   3. --require-neon, which fails the run if the NEON path is absent. CI uses it
//      on the arm64 job only, so exactly one job can claim real coverage.
//   4. --simulate-mismatch, which corrupts one scan value on purpose. A gate that
//      has never been observed to fail is not known to be a gate; CI runs this to
//      prove the check can actually go red.
//
// Build:  clang++ -std=c++20 -O2 -I../src bench_parity.cpp -o bench_parity
// Run:    ./bench_parity [--require-neon] [--simulate-mismatch]
// Exit:   0 = pass, 1 = numerical/ranking diff failed, 2 = --require-neon unmet

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api.hpp"
#include "common.hpp"
#include "dot.hpp"
#include "recall_op.hpp"

int main(int argc, char** argv) {
    bool require_neon = false;
    bool simulate_mismatch = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--require-neon") == 0) require_neon = true;
        if (std::strcmp(argv[i], "--simulate-mismatch") == 0) simulate_mismatch = true;
    }

    // Same demo profile the native driver uses (food + travel blend), so this
    // target and ./shuashua are checking the identical query.
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    weights[0] = 0.5f;  // food
    weights[2] = 0.5f;  // travel

    const SyntheticData& data = shared_data();
    const std::vector<float> query = make_query(weights, data.centroids);
    const float* q = query.data();
    const std::size_t k = kRecallK;
    const ItemStore& store = data.store;

    bench::title("parity: naive vs SIMD recall kernel");
    std::printf("  store            : %zu items, DIM=%zu, k=%zu\n",
                store.count(), ItemStore::DIM, k);
    std::printf("  kernel backend   : %s\n", bench::kernel_backend());
    if (!bench::neon_active()) {
        std::printf("  >> dot_simd compiles to `return dot_scalar(...)` on this target\n");
        std::printf("  >> (dot.hpp:86-90) so the diff below is TRIVIALLY TRUE, not evidence\n");
        std::printf("  >> that the hand-written NEON kernel is correct.\n");
    }

    // --- Numerical parity: do the two kernels compute the same dot products? ---
    // score_all scans items in id order, so these two vectors are id-aligned.
    const std::vector<Scored> scan_naive = score_all(store, q, dot_scalar);
    std::vector<Scored> scan_simd = score_all(store, q, dot_simd);
    if (simulate_mismatch && !scan_simd.empty()) {
        // Deliberately corrupt one value to prove the gate below can go red.
        scan_simd[scan_simd.size() / 2].score += 1.0f;
        std::printf("\n  !! --simulate-mismatch: corrupted item %zu by +1.0 on purpose\n",
                    scan_simd.size() / 2);
    }
    float max_delta = 0.0f;
    std::size_t worst_id = 0;
    for (std::size_t i = 0; i < scan_naive.size(); ++i) {
        const float d = std::fabs(scan_naive[i].score - scan_simd[i].score);
        if (d > max_delta) {
            max_delta = d;
            worst_id = i;
        }
    }
    std::size_t exact_matches = 0;
    for (std::size_t i = 0; i < scan_naive.size(); ++i) {
        if (scan_naive[i].score == scan_simd[i].score) ++exact_matches;
    }

    // --- Ranking parity: identical top-k order? and (looser) the same items? ---
    const std::vector<Scored> rank_naive = recall_naive(store, q, k);
    const std::vector<Scored> rank_simd = recall_simd(store, q, k);
    std::size_t positional_diff = 0;
    const std::size_t m = std::min(rank_naive.size(), rank_simd.size());
    for (std::size_t i = 0; i < m; ++i) {
        if (rank_naive[i].id != rank_simd[i].id) ++positional_diff;
    }
    std::vector<std::uint32_t> ids_naive;
    std::vector<std::uint32_t> ids_simd;
    for (const Scored& s : rank_naive) ids_naive.push_back(s.id);
    for (const Scored& s : rank_simd) ids_simd.push_back(s.id);
    std::sort(ids_naive.begin(), ids_naive.end());
    std::sort(ids_simd.begin(), ids_simd.end());
    const bool same_set = (ids_naive == ids_simd);

    std::printf("\n  -- numerical diff over ALL %zu items --\n", scan_naive.size());
    std::printf("  max |naive - simd|                             %.3e  (worst item id %zu)\n",
                static_cast<double>(max_delta), worst_id);
    std::printf("  bit-identical results                          %zu / %zu\n",
                exact_matches, scan_naive.size());
    std::printf("  tolerance                                      %.0e\n", 1e-4);

    std::printf("\n  -- ranking diff over top-%zu --\n", k);
    std::printf("  positional differences                         %zu\n", positional_diff);
    std::printf("  same item set                                  %s\n", same_set ? "yes" : "no");

    // --- Speedup. Reported as min-of-rounds, interleaved, so the two paths see
    // the same machine conditions. Two granularities:
    //   scan  = what SIMD actually touches
    //   recall= scan + the SHARED, un-vectorised top-k sort (the honest number)
    constexpr int kRounds = 30, kIters = 100;
    const auto scan = bench::best_us_interleaved(2, kRounds, kIters, [&](int v) {
        return v == 0 ? score_all(store, q, dot_scalar).front().score
                      : score_all(store, q, dot_simd).front().score;
    });
    const auto full = bench::best_us_interleaved(2, kRounds, kIters, [&](int v) {
        return v == 0 ? recall_naive(store, q, k).front().score
                      : recall_simd(store, q, k).front().score;
    });
    bench::keep_sink();

    std::printf("\n  -- speedup --\n");
    bench::harness(kRounds, kIters);
    bench::row("dot scan, naive", scan[0]);
    bench::row("dot scan, simd", scan[1]);
    bench::row_x("scan speedup", scan[0] / scan[1]);
    bench::row("end-to-end recall, naive", full[0]);
    bench::row("end-to-end recall, simd", full[1]);
    bench::row_x("recall speedup (top-k sort is shared)", full[0] / full[1]);

    const bool diff_ok = (max_delta < 1e-4f) && same_set;
    std::printf("\n  VERDICT: %s", diff_ok ? "PASS" : "FAIL");
    if (diff_ok && !bench::neon_active()) std::printf("  (trivial — no NEON on this target)");
    std::printf("\n");

    if (!diff_ok) {
        std::printf("  ERROR: kernels disagree beyond tolerance. Exit 1.\n");
        return 1;
    }
    if (require_neon && !bench::neon_active()) {
        std::printf("  ERROR: --require-neon given but __ARM_NEON is not defined, so the\n");
        std::printf("         NEON kernel was never exercised. Exit 2.\n");
        return 2;
    }
    return 0;
}
