// bench/bench_layout.cpp — what does SoA actually buy, separately from SIMD?
//
// These are two DIFFERENT benefits and conflating them is an easy way to overclaim:
//
//   * SIMD needs each item's DIM floats to be CONTIGUOUS, because dot_simd strides
//     along the dimension (dot.hpp:57). An AoS struct with `float vec[DIM]` inside
//     satisfies that just as well as a flat buffer does.
//   * SoA separately avoids pulling per-item METADATA through cache during a scan,
//     because the metadata lives in a parallel array (item_store.hpp:23-24) instead
//     of being interleaved with the vectors.
//
// This target isolates the second effect: both layouts keep the DIM floats
// contiguous and run the IDENTICAL dot_simd, so any difference is cache/bandwidth,
// not vectorisation.
//
// Build: clang++ -std=c++20 -O2 -I../src bench_layout.cpp -o bench_layout

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "api.hpp"
#include "common.hpp"
#include "dot.hpp"

// AoS mirror: the embedding interleaved with exactly the fields Note carries.
struct ItemAoS {
    float         vec[ItemStore::DIM];  // still contiguous -> dot_simd is unchanged
    std::uint32_t id;
    std::uint8_t  category;
    float         popularity;
    std::uint32_t age_days;
};

int main() {
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    weights[0] = 0.5f;
    weights[2] = 0.5f;
    const SyntheticData& data = shared_data();
    const std::vector<float> query = make_query(weights, data.centroids);
    const float* q = query.data();
    const ItemStore& store = data.store;
    const std::size_t n = store.count();

    std::vector<ItemAoS> aos(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* v = store.vector_of(static_cast<std::uint32_t>(i));
        std::copy(v, v + ItemStore::DIM, aos[i].vec);
        aos[i].id = store.notes[i].id;
        aos[i].category = store.notes[i].category;
        aos[i].popularity = store.notes[i].popularity;
        aos[i].age_days = store.notes[i].age_days;
    }

    const std::size_t soa_bytes = n * ItemStore::DIM * sizeof(float);
    const std::size_t aos_bytes = n * sizeof(ItemAoS);

    bench::title("layout — SoA vs AoS, same SIMD kernel");
    std::printf("  store          : %zu items, DIM=%zu\n", n, ItemStore::DIM);
    std::printf("  kernel backend : %s\n", bench::kernel_backend());
    std::printf("  SoA bytes touched per scan  : %zu\n", soa_bytes);
    std::printf("  sizeof(ItemAoS)             : %zu\n", sizeof(ItemAoS));
    std::printf("  AoS bytes touched per scan  : %zu  (+%.1f%%)\n\n", aos_bytes,
                100.0 * static_cast<double>(aos_bytes - soa_bytes) /
                    static_cast<double>(soa_bytes));

    // The effect being measured here is a few percent at most, which is the same
    // order as run-to-run drift on a laptop. A single min-of-rounds figure would
    // therefore be untrustworthy — and in practice the sign of the difference
    // flips between runs. So: repeat the whole interleaved measurement as several
    // INDEPENDENT trials and report the spread, letting the data say whether the
    // effect is resolvable at all rather than asserting that it is.
    constexpr int kTrials = 9, kRounds = 20, kIters = 200;
    std::vector<double> soa(kTrials), ao(kTrials), adv_pct(kTrials);
    for (int trial = 0; trial < kTrials; ++trial) {
        const auto t = bench::best_us_interleaved(2, kRounds, kIters, [&](int v) {
            float acc = 0.0f;
            if (v == 0) {
                for (std::size_t i = 0; i < n; ++i)
                    acc += dot_simd(q, store.vector_of(static_cast<std::uint32_t>(i)),
                                    ItemStore::DIM);
            } else {
                for (std::size_t i = 0; i < n; ++i) acc += dot_simd(q, aos[i].vec, ItemStore::DIM);
            }
            return static_cast<double>(acc);
        });
        soa[static_cast<std::size_t>(trial)] = t[0];
        ao[static_cast<std::size_t>(trial)] = t[1];
        adv_pct[static_cast<std::size_t>(trial)] = 100.0 * (t[1] - t[0]) / t[1];
    }
    bench::keep_sink();

    std::vector<double> sorted_adv = adv_pct;
    std::sort(sorted_adv.begin(), sorted_adv.end());
    std::vector<double> s_soa = soa, s_aos = ao;
    std::sort(s_soa.begin(), s_soa.end());
    std::sort(s_aos.begin(), s_aos.end());
    const double med_soa = s_soa[kTrials / 2];
    const double med_aos = s_aos[kTrials / 2];
    const double med_adv = sorted_adv[kTrials / 2];
    const double lo_adv = sorted_adv.front();
    const double hi_adv = sorted_adv.back();
    int soa_wins = 0;
    for (double a : adv_pct) {
        if (a > 0) ++soa_wins;
    }

    std::printf("  harness: %d independent trials, each min of %d rounds x %d iters,\n", kTrials,
                kRounds, kIters);
    std::printf("           variants interleaved within every round\n\n");
    bench::row("SoA scan, median of trials", med_soa);
    bench::row("AoS scan, median of trials", med_aos);
    bench::row_pct("SoA advantage, median", med_adv);
    std::printf("  %-46s %7.1f%% .. %.1f%%\n", "SoA advantage, full spread", lo_adv, hi_adv);
    std::printf("  %-46s %d / %d trials\n", "trials where SoA was faster", soa_wins, kTrials);
    std::printf("\n");
    if (lo_adv < 0.0 && hi_adv > 0.0) {
        bench::note("The spread STRADDLES ZERO: at this store size the layout");
        bench::note("difference is NOT RESOLVABLE above run-to-run noise. Do not");
        bench::note("quote a percentage for it; quote that it is unmeasurable here.");
    } else {
        bench::note("The spread keeps one sign across all trials, so the effect is");
        bench::note("resolvable at this size — but it is still only a few percent.");
    }
    std::printf("\n");
    bench::note("AoS runs the SAME dot_simd at nearly the same speed, because");
    bench::note("`float vec[DIM]` is contiguous there too. Vectorisation needs");
    bench::note("contiguous embedding blocks; it does NOT need SoA.");
    bench::note("SoA's own benefit is the metadata it keeps out of cache, and at");
    bench::note("this store size the whole thing fits in L2, so it stays small.");
    return 0;
}
