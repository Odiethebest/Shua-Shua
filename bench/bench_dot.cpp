// bench/bench_dot.cpp — the dot kernel at THREE granularities.
//
// Why three: the naive-vs-SIMD speedup people quote is extremely sensitive to what
// else is inside the timing loop. Reporting one number invites the question "one
// number measured how?". These three bracket it:
//
//   [1] one dot        — the kernel alone, 64 floats, nothing else.
//   [2] full scan      — 3,000 dots, accumulating into a scalar. No allocation.
//   [3] score_all()    — what RecallOp actually calls: the same 3,000 dots PLUS
//                        building a 3,000-element std::vector<Scored>.
//
// The speedup shrinks as you move down the list, because the added work is not
// vectorised. [3] is the honest "what the pipeline pays" figure; [1]/[2] are the
// honest "what the kernel is worth" figures. Quote whichever you mean, and say which.
//
// Build: clang++ -std=c++20 -O2 -I../src bench_dot.cpp -o bench_dot

#include <cstddef>
#include <cstdio>
#include <vector>

#include "api.hpp"
#include "common.hpp"
#include "dot.hpp"
#include "recall_op.hpp"

int main() {
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    weights[0] = 0.5f;
    weights[2] = 0.5f;
    const SyntheticData& data = shared_data();
    const std::vector<float> query = make_query(weights, data.centroids);
    const float* q = query.data();
    const ItemStore& store = data.store;
    const std::size_t n = store.count();

    bench::title("dot kernel — three granularities");
    std::printf("  store          : %zu items, DIM=%zu\n", n, ItemStore::DIM);
    std::printf("  kernel backend : %s\n", bench::kernel_backend());
    std::printf("  vectorisation  : across DIM (dot.hpp:57 strides d by 4), NOT across items\n");
    std::printf("                   -> one horizontal reduction per item (dot.hpp:74-75)\n");
    std::printf("  work per scan  : %zu dots x %zu dims = %zu multiply-adds\n\n",
                n, ItemStore::DIM, n * ItemStore::DIM);

    constexpr int kRounds = 30;

    // [1] a single dot, hammered. Item 0's vector against the query.
    const float* v0 = store.vector_of(0);
    const auto one = bench::best_us_interleaved(2, kRounds, 20000, [&](int v) {
        return v == 0 ? dot_scalar(q, v0, ItemStore::DIM) : dot_simd(q, v0, ItemStore::DIM);
    });

    // [2] full scan, no allocation.
    const auto scan = bench::best_us_interleaved(2, kRounds, 200, [&](int v) {
        float acc = 0.0f;
        if (v == 0) {
            for (std::size_t i = 0; i < n; ++i)
                acc += dot_scalar(q, store.vector_of(static_cast<std::uint32_t>(i)), ItemStore::DIM);
        } else {
            for (std::size_t i = 0; i < n; ++i)
                acc += dot_simd(q, store.vector_of(static_cast<std::uint32_t>(i)), ItemStore::DIM);
        }
        return static_cast<double>(acc);
    });

    // [3] score_all — the scan RecallOp really performs, vector build included.
    const auto sall = bench::best_us_interleaved(2, kRounds, 200, [&](int v) {
        return v == 0 ? score_all(store, q, dot_scalar).front().score
                      : score_all(store, q, dot_simd).front().score;
    });
    bench::keep_sink();

    bench::harness(kRounds, 200);
    std::printf("\n  [1] one dot (64 floats)\n");
    std::printf("  %-46s %9.4f us\n", "scalar", one[0]);
    std::printf("  %-46s %9.4f us\n", "simd", one[1]);
    bench::row_x("speedup", one[0] / one[1]);

    std::printf("\n  [2] full scan of %zu items, no allocation\n", n);
    bench::row("scalar", scan[0]);
    bench::row("simd", scan[1]);
    bench::row_x("speedup", scan[0] / scan[1]);

    std::printf("\n  [3] score_all() — scan + build vector<Scored>\n");
    bench::row("scalar", sall[0]);
    bench::row("simd", sall[1]);
    bench::row_x("speedup", sall[0] / sall[1]);

    std::printf("\n  allocation overhead inside score_all (simd)    %9.2f us  (%.0f%% of [3])\n",
                sall[1] - scan[1], 100.0 * (sall[1] - scan[1]) / sall[1]);
    bench::note("speedup falls from [1] to [3] because the vector build is not vectorised.");
    return 0;
}
