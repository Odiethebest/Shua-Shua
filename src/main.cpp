// -----------------------------------------------------------------------------
// Shua Shua — native kernel (Milestone M2).
//
// M2 scope: everything M1 does (Recall -> Feature -> Score -> Rerank through the
// DagScheduler, feed + trace), now with the hand-written SIMD recall kernel and
// a parity + speedup check that proves the SIMD path matches the naive reference.
//
// Build:
//   clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
// -----------------------------------------------------------------------------

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <vector>

#include "dot.hpp"
#include "feature_op.hpp"
#include "item_store.hpp"
#include "note.hpp"
#include "operator.hpp"
#include "rerank_op.hpp"
#include "recall_op.hpp"
#include "scheduler.hpp"
#include "score_op.hpp"
#include "synthetic.hpp"

namespace {

// Human-readable category labels, indexed by Note::category. Presentation only —
// the engine only ever sees the numeric category.
constexpr const char* CATEGORY_NAMES[] = {
    "food", "fashion", "travel", "tech", "fitness", "beauty",
};
constexpr std::uint8_t NUM_CATEGORIES =
    static_cast<std::uint8_t>(sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]));

// A persona: what the user is into. The per-category weights drive BOTH the
// recall query (a blended centroid) and the category_match feature, so one
// profile shapes recall and ranking — as it would in a real system.
//
// WHY a blended (not single-category) persona: it gives recall a mix of
// categories to return, so the diversity rerank has something real to do. A
// pure single-category query would make the whole page one category and the
// rerank a visible no-op.
struct Persona {
    std::vector<float> category_weights;  // per category, sums to 1
    std::vector<float> query;             // blended, unit-normalized query vector
};

Persona make_persona(const SyntheticData& data) {
    Persona p;
    p.category_weights.assign(NUM_CATEGORIES, 0.0f);
    p.category_weights[0] = 0.5f;  // food
    p.category_weights[2] = 0.5f;  // travel

    // query = weighted sum of category centroids, then unit-normalized so it is
    // directly comparable to the (also unit) item vectors.
    p.query.assign(ItemStore::DIM, 0.0f);
    for (std::uint8_t c = 0; c < NUM_CATEGORIES; ++c) {
        const float w = p.category_weights[c];
        for (std::size_t d = 0; d < ItemStore::DIM; ++d) {
            p.query[d] += w * data.centroids[c][d];
        }
    }
    normalize(p.query.data(), ItemStore::DIM);
    return p;
}

// Seed batch = every item in the store. This represents the candidate pool recall
// draws from; its size is what the trace reports as RecallOp's in_count, giving
// the funnel its top number.
Batch full_pool(const ItemStore& store) {
    Batch pool;
    pool.items.reserve(store.count());
    for (std::size_t i = 0; i < store.count(); ++i) {
        Candidate c;
        c.id = static_cast<std::uint32_t>(i);
        pool.items.push_back(c);
    }
    return pool;
}

void print_feed(const Batch& feed, const ItemStore& store) {
    std::cout << "\nFinal feed (" << feed.items.size() << " notes):\n";
    for (std::size_t rank = 0; rank < feed.items.size(); ++rank) {
        const Candidate& c = feed.items[rank];
        const Note& note = store.notes[c.id];
        std::cout << "  #" << (rank + 1)
                  << "  id=" << c.id
                  << "  cat=" << CATEGORY_NAMES[note.category]
                  << "  score=" << c.score
                  << "  (sim=" << c.similarity
                  << " rec=" << c.recency
                  << " pop=" << c.popularity << ")\n";
    }
}

void print_trace(const std::vector<TraceEntry>& trace) {
    std::cout << "\nDAG trace (per operator):\n";
    for (const TraceEntry& e : trace) {
        std::cout << "  " << e.name
                  << "  in=" << e.in_count
                  << " out=" << e.out_count
                  << " latency=" << e.latency_us << "us"
                  << " sample_ids=[";
        for (std::size_t i = 0; i < e.sample_ids.size(); ++i) {
            std::cout << e.sample_ids[i];
            if (i + 1 < e.sample_ids.size()) {
                std::cout << ",";
            }
        }
        std::cout << "]\n";
    }
}

// M2: prove the SIMD recall kernel matches the naive reference, and measure the
// speedup. Runs both kernels on the same input and reports (a) whether the top-k
// rankings agree, (b) the largest similarity difference across ALL items — which
// should be at the floating-point-reassociation level, ~1e-7 — and (c) timings.
void run_recall_diagnostics(const ItemStore& store, const std::vector<float>& query,
                            std::size_t k) {
    const float* q = query.data();

    // --- Numerical parity: do the two kernels compute the same dot products? ---
    // score_all scans items in id order, so these two vectors are id-aligned and
    // can be compared element by element.
    const std::vector<Scored> scan_naive = score_all(store, q, dot_scalar);
    const std::vector<Scored> scan_simd = score_all(store, q, dot_simd);
    float max_delta = 0.0f;
    for (std::size_t i = 0; i < scan_naive.size(); ++i) {
        max_delta = std::max(max_delta, std::fabs(scan_naive[i].score - scan_simd[i].score));
    }

    // --- Ranking parity: identical top-k order? and (looser) the same items? ---
    const std::vector<Scored> rank_naive = recall_naive(store, q, k);
    const std::vector<Scored> rank_simd = recall_simd(store, q, k);
    std::size_t positional_diff = 0;
    const std::size_t m = std::min(rank_naive.size(), rank_simd.size());
    for (std::size_t i = 0; i < m; ++i) {
        if (rank_naive[i].id != rank_simd[i].id) {
            ++positional_diff;
        }
    }
    std::vector<std::uint32_t> ids_naive;
    std::vector<std::uint32_t> ids_simd;
    for (const Scored& s : rank_naive) ids_naive.push_back(s.id);
    for (const Scored& s : rank_simd) ids_simd.push_back(s.id);
    std::sort(ids_naive.begin(), ids_naive.end());
    std::sort(ids_simd.begin(), ids_simd.end());
    const bool same_set = (ids_naive == ids_simd);

    // --- Speedup. Time each kernel over many iterations for a stable average. ---
    // Two measurements: the scan alone (what SIMD actually accelerates) and the
    // full recall (scan + the shared, un-vectorized top-k sort — an honest
    // end-to-end number that Amdahl's law holds below the raw kernel speedup).
    double sink = 0.0;  // accumulate results so the timed calls are not elided
    constexpr int kIters = 200;
    const auto now = []() { return std::chrono::steady_clock::now(); };
    const auto avg_us = [](std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<double, std::micro>(b - a).count() / kIters;
    };

    const auto s0 = now();
    for (int i = 0; i < kIters; ++i) { sink += score_all(store, q, dot_scalar).front().score; }
    const auto s1 = now();
    for (int i = 0; i < kIters; ++i) { sink += score_all(store, q, dot_simd).front().score; }
    const auto s2 = now();
    for (int i = 0; i < kIters; ++i) { sink += recall_naive(store, q, k).front().score; }
    const auto s3 = now();
    for (int i = 0; i < kIters; ++i) { sink += recall_simd(store, q, k).front().score; }
    const auto s4 = now();

    const double scan_naive_us = avg_us(s0, s1);
    const double scan_simd_us = avg_us(s1, s2);
    const double rec_naive_us = avg_us(s2, s3);
    const double rec_simd_us = avg_us(s3, s4);
    volatile double keep = sink;  // observable use so the loops above survive -O2
    (void)keep;

    // --- Report ---
    std::cout << "\n=== M2: recall kernel parity + speedup ("
              << store.count() << " items, DIM=" << ItemStore::DIM << ") ===\n";
    std::cout << std::fixed << std::setprecision(2)
              << "  dot scan:          naive " << scan_naive_us << "us | simd "
              << scan_simd_us << "us | speedup " << (scan_naive_us / scan_simd_us) << "x\n"
              << "  end-to-end recall: naive " << rec_naive_us << "us | simd "
              << rec_simd_us << "us | speedup " << (rec_naive_us / rec_simd_us)
              << "x (k=" << k << ", top-k sort shared)\n"
              << std::defaultfloat;
    std::cout << "  result diff = " << positional_diff
              << (positional_diff == 0 ? " (top-" : " near-tie swaps (top-")
              << k << " ranking " << (positional_diff == 0 ? "identical" : "same items, reordered")
              << "), same item set = " << (same_set ? "yes" : "no") << "\n";
    std::cout << "  max similarity delta over all items = " << std::scientific
              << std::setprecision(2) << max_delta << std::defaultfloat
              << " (floating-point reassociation only)\n";

    // The result is equivalent when the kernels agree numerically and recall the
    // same items; any positional difference is then necessarily a near-tie swap.
    const bool pass = (max_delta < 1e-4f) && same_set;
    std::cout << "  verdict: " << (pass ? "PASS" : "FAIL") << "\n";
}

}  // namespace

int main() {
    constexpr std::uint32_t per_category = 500;
    constexpr std::uint32_t seed = 42;
    constexpr std::size_t   recall_k = 300;  // shared by the pipeline and the parity check

    const SyntheticData data = build_synthetic_data(NUM_CATEGORIES, per_category, seed);
    const ItemStore& store = data.store;
    const Persona persona = make_persona(data);

    // Build the cascade. Cardinalities shrink stage by stage: pool -> recall ->
    // (features attached, count unchanged) -> score top-k -> final page. Recall
    // runs on the SIMD kernel; the parity check below proves it matches naive.
    DagScheduler pipeline;
    pipeline.add(std::make_unique<RecallOp>(store, persona.query, recall_k, RecallKernel::Simd));
    pipeline.add(std::make_unique<FeatureOp>(store, persona.category_weights));
    pipeline.add(std::make_unique<ScoreOp>(
        ScoreOp::Weights{/*similarity=*/1.0f, /*category_match=*/0.5f,
                         /*recency=*/0.3f, /*popularity=*/0.2f},
        /*k=*/50));
    pipeline.add(std::make_unique<RerankOp>(store, /*page_size=*/12, /*lambda=*/0.7f));

    std::vector<TraceEntry> trace;
    const Batch feed = pipeline.run(full_pool(store), trace);

    std::cout << "Shua Shua - M2: DAG + SIMD recall\n";
    std::cout << "Store: " << store.count() << " notes, "
              << static_cast<int>(NUM_CATEGORIES) << " categories, DIM="
              << ItemStore::DIM << "\n";
    std::cout << "Persona: 50% food + 50% travel\n";

    print_feed(feed, store);
    print_trace(trace);
    run_recall_diagnostics(store, persona.query, recall_k);
    return 0;
}
