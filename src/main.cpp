// -----------------------------------------------------------------------------
// Shua Shua — native driver (Milestone M3, Part A).
//
// The engine orchestration now lives in api.hpp behind a single recommend()
// entry point that returns the feed + trace, plus to_json() that serializes them
// — the exact contract the WASM boundary (M3 Part B) will expose to JS. This file
// is just the NATIVE front door: it calls recommend(), pretty-prints the feed and
// DAG trace for a human, dumps the JSON payload, and runs the M2 recall
// parity/speedup check as a dev diagnostic.
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
#include <vector>

#include "api.hpp"
#include "dot.hpp"
#include "item_store.hpp"
#include "note.hpp"
#include "operator.hpp"
#include "recall_op.hpp"

namespace {

void print_feed(const Batch& feed, const ItemStore& store) {
    std::cout << "\nFinal feed (" << feed.items.size() << " notes):\n";
    for (std::size_t rank = 0; rank < feed.items.size(); ++rank) {
        const Candidate& c = feed.items[rank];
        const Note& note = store.notes[c.id];
        std::cout << "  #" << (rank + 1)
                  << "  id=" << c.id
                  << "  cat=" << category_name(note.category)
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
    // score_all scans items in id order, so these two vectors are id-aligned.
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

    // --- Speedup: scan alone (what SIMD accelerates) and full recall (scan +
    // the shared, un-vectorized top-k sort — an honest end-to-end number). ---
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

    const bool pass = (max_delta < 1e-4f) && same_set;
    std::cout << "  verdict: " << (pass ? "PASS" : "FAIL") << "\n";
}

}  // namespace

int main() {
    constexpr int persona_id = 2;  // "Foodie + Traveler" — a blend, so rerank has work

    const Recommendation rec = recommend(persona_id);
    const SyntheticData& data = shared_data();

    std::cout << "Shua Shua - M3: recommend() + JSON boundary\n";
    std::cout << "Store: " << data.store.count() << " notes, "
              << static_cast<int>(NUM_CATEGORIES) << " categories, DIM="
              << ItemStore::DIM << "\n";
    std::cout << "Persona: " << rec.persona_label << "\n";

    print_feed(rec.feed, data.store);
    print_trace(rec.trace);

    // The exact payload the WASM boundary will hand to JS.
    std::cout << "\n--- recommend() JSON (WASM boundary output) ---\n";
    std::cout << to_json(rec) << "\n";

    // Dev diagnostic: SIMD vs naive recall parity + speedup (M2).
    const std::vector<float> query =
        make_query(personas()[persona_id].category_weights, data.centroids);
    run_recall_diagnostics(data.store, query, kRecallK);
    return 0;
}
