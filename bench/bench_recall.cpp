// bench/bench_recall.cpp — where does RecallOp's time actually go?
//
// RecallOp = scan (score_all) + rank (rank_topk). M2 optimised the scan with SIMD.
// This target asks the follow-up question: after that optimisation, what dominates?
//
// It also prices two top-k strategies the current code does NOT use. rank_topk
// (recall_op.hpp:63-73) does a FULL std::sort over every candidate and then
// resizes to k; its comment calls partial_sort "premature now". This measures
// whether that is still true at the current store size.
//
// The alternatives compute the SAME top-k with the SAME comparator (score desc,
// id asc), so this is a like-for-like swap, not a change of semantics.
//
// Build: clang++ -std=c++20 -O2 -I../src bench_recall.cpp -o bench_recall

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "api.hpp"
#include "common.hpp"
#include "dot.hpp"
#include "recall_op.hpp"

// The exact comparator rank_topk uses (recall_op.hpp:64-69).
static bool better(const Scored& a, const Scored& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.id < b.id;
}

int main() {
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    weights[0] = 0.5f;
    weights[2] = 0.5f;
    const SyntheticData& data = shared_data();
    const std::vector<float> query = make_query(weights, data.centroids);
    const float* q = query.data();
    const ItemStore& store = data.store;
    const std::size_t k = kRecallK;

    bench::title("recall breakdown — scan vs top-k, and top-k strategies");
    std::printf("  store          : %zu items, DIM=%zu, k=%zu\n", store.count(), ItemStore::DIM, k);
    std::printf("  kernel backend : %s\n\n", bench::kernel_backend());

    constexpr int kRounds = 30, kIters = 100;

    // 0 scalar scan | 1 simd scan | 2 scalar+sort (pre-M2) | 3 simd+sort (today)
    // 4 simd+partial_sort | 5 simd+nth_element+sort(k)
    const auto t = bench::best_us_interleaved(6, kRounds, kIters, [&](int v) {
        switch (v) {
            case 0: return score_all(store, q, dot_scalar).front().score;
            case 1: return score_all(store, q, dot_simd).front().score;
            case 2: {
                auto s = score_all(store, q, dot_scalar);
                std::sort(s.begin(), s.end(), better);
                s.resize(k);
                return s.front().score;
            }
            case 3: {
                auto s = score_all(store, q, dot_simd);
                std::sort(s.begin(), s.end(), better);
                s.resize(k);
                return s.front().score;
            }
            case 4: {
                auto s = score_all(store, q, dot_simd);
                std::partial_sort(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), s.end(),
                                  better);
                s.resize(k);
                return s.front().score;
            }
            default: {
                auto s = score_all(store, q, dot_simd);
                std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k), s.end(),
                                 better);
                s.resize(k);
                std::sort(s.begin(), s.end(), better);
                return s.front().score;
            }
        }
    });
    bench::keep_sink();

    bench::harness(kRounds, kIters);
    std::printf("\n");
    bench::row("[1] scalar scan only", t[0]);
    bench::row("[2] simd scan only", t[1]);
    bench::row("[3] scalar scan + std::sort   (pre-M2)", t[2]);
    bench::row("[4] simd scan + std::sort     (CURRENT)", t[3]);
    bench::row("[5] simd scan + partial_sort", t[4]);
    bench::row("[6] simd scan + nth_element + sort(k)", t[5]);

    std::printf("\n");
    bench::row("top-k cost inside CURRENT recall  [4]-[2]", t[3] - t[1]);
    bench::row_pct("  ... as a share of CURRENT recall", 100.0 * (t[3] - t[1]) / t[3]);
    bench::row_x("scan speedup from SIMD            [1]/[2]", t[0] / t[1]);
    bench::row_x("recall speedup M2 delivered       [3]/[4]", t[2] / t[3]);
    bench::row_x("recall speedup nth_element adds   [4]/[6]", t[3] / t[5]);

    // Verify the cheap strategies really do produce the identical top-k, so the
    // speedup above is not being bought with different results.
    auto ref = score_all(store, q, dot_simd);
    std::sort(ref.begin(), ref.end(), better);
    ref.resize(k);
    auto alt = score_all(store, q, dot_simd);
    std::nth_element(alt.begin(), alt.begin() + static_cast<std::ptrdiff_t>(k), alt.end(), better);
    alt.resize(k);
    std::sort(alt.begin(), alt.end(), better);
    bool identical = ref.size() == alt.size();
    for (std::size_t i = 0; identical && i < ref.size(); ++i) {
        if (ref[i].id != alt[i].id) identical = false;
    }
    std::printf("\n  nth_element produces the identical top-%zu order: %s\n", k,
                identical ? "yes" : "NO");
    return identical ? 0 : 1;
}
