// bench/bench_pipeline.cpp — end-to-end cost of one recommend_from_profile(),
// and how much of it the DAG trace does NOT account for.
//
// The trace is the product, so it matters that its coverage is known. Three costs
// sit structurally OUTSIDE every operator's timer, because Operator::run() only
// brackets transform() (operator.hpp:83-85):
//
//   * full_pool()          — built as an ARGUMENT at api.hpp:140, before run()
//   * the scheduler's copy — `Batch batch = seed;` at scheduler.hpp:33
//   * to_json()            — called after the pipeline, at bindings.cpp:54
//
// None of these is a bug; they are the price of a uniform in_count contract and of
// a string boundary. This target just makes the price visible instead of implicit.
//
// Build: clang++ -std=c++20 -O2 -I../src bench_pipeline.cpp -o bench_pipeline

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "api.hpp"
#include "common.hpp"

int main() {
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    weights[0] = 0.5f;
    weights[2] = 0.5f;

    const SyntheticData& data = shared_data();  // pay the one-time store build first

    bench::title("pipeline — end-to-end, and what the trace misses");
    std::printf("  store          : %zu items, DIM=%zu\n", data.store.count(), ItemStore::DIM);
    std::printf("  kernel backend : %s\n", bench::kernel_backend());
    std::printf("  page shape     : %zu -> %zu -> %zu -> %zu -> %zu\n\n", data.store.count(),
                kRecallK, kScoreK, kRerankPool, kPageSize);

    constexpr int kRounds = 30, kIters = 100;
    constexpr int kCalls = 2000;

    // The trace total and the call total MUST come from the same observations.
    // Timing the call with min-of-rounds while averaging trace latencies over a
    // different loop is not a like-for-like comparison and can even make
    // "unaccounted" come out negative. So: one loop, both numbers, means of the
    // SAME calls. Per call the operator timers are nested strictly inside the
    // call, so traced <= wall holds for every sample and therefore for the mean.
    double op_us[8] = {0};
    std::string op_name[8];
    std::size_t op_in[8] = {0}, op_out[8] = {0};
    std::size_t n_ops = 0;
    double wall_sum = 0.0;
    double wall_min = 1e18;

    for (int i = 0; i < 200; ++i) (void)recommend_from_profile(weights);  // warm

    for (int it = 0; it < kCalls; ++it) {
        const auto s = bench::clk::now();
        const Recommendation r = recommend_from_profile(weights);
        const auto e = bench::clk::now();
        const double wall = bench::us(s, e);
        wall_sum += wall;
        if (wall < wall_min) wall_min = wall;
        n_ops = r.trace.size();
        for (std::size_t i = 0; i < r.trace.size() && i < 8; ++i) {
            op_us[i] += r.trace[i].latency_us;
            op_name[i] = r.trace[i].name;
            op_in[i] = r.trace[i].in_count;
            op_out[i] = r.trace[i].out_count;
        }
        bench::g_sink += static_cast<double>(r.feed.items.size());
    }
    for (std::size_t i = 0; i < n_ops && i < 8; ++i) op_us[i] /= kCalls;
    const double total_us = wall_sum / kCalls;

    // The untraced pieces, timed separately (min-of-rounds) to price them.
    // NOTE these are a different harness from the means above; they are here to
    // show WHERE the unaccounted time goes, not to be subtracted from it exactly.
    const Batch seed_for_copy = full_pool(data.store);
    const auto pool_us = bench::best_us(kRounds, kIters, [&] {
        const Batch p = full_pool(data.store);
        // Touch the payload, else the copy is dead code and gets elided.
        return static_cast<double>(p.items[p.items.size() / 2].id);
    });
    const auto copy_us = bench::best_us(kRounds, kIters, [&] {
        const Batch b = seed_for_copy;
        return static_cast<double>(b.items[b.items.size() / 2].id);
    });
    const Recommendation held = recommend_from_profile(weights);
    const auto json_us = bench::best_us(kRounds, kIters, [&] {
        const std::string j = to_json(held);
        return static_cast<double>(j.size());
    });
    bench::keep_sink();

    double traced = 0;
    std::printf("  harness: mean of %d calls (after %d warm-up), trace sum and wall\n", kCalls,
                200);
    std::printf("           clock taken from the SAME calls\n\n");
    std::printf("  -- per operator (as the trace reports) --\n");
    for (std::size_t i = 0; i < n_ops && i < 8; ++i) {
        char label[96];
        std::snprintf(label, sizeof(label), "%-10s %5zu -> %-5zu", op_name[i].c_str(), op_in[i],
                      op_out[i]);
        bench::row(label, op_us[i]);
        traced += op_us[i];
    }
    bench::row("SUM of trace latencies", traced);

    std::printf("\n  -- totals, same calls --\n");
    bench::row("recommend_from_profile() wall, mean", total_us);
    bench::row("  (best single call observed)", wall_min);
    bench::row("  UNACCOUNTED by the trace", total_us - traced);
    bench::row_pct("  trace coverage of the call", 100.0 * traced / total_us);

    std::printf("\n  -- where the unaccounted time goes (min-of-rounds, separate harness) --\n");
    bench::row("full_pool()            (api.hpp:140)", pool_us);
    bench::row("seed copy              (scheduler.hpp:33)", copy_us);
    bench::row("to_json()              (bindings.cpp:54)", json_us);
    bench::note("to_json runs AFTER the pipeline (bindings.cpp:54), so it is not part");
    bench::note("of the call above at all — it is additional cost the trace never sees.");
    bench::row("full C++ cost per request (call + to_json)", total_us + json_us);
    bench::row_pct("  trace coverage incl. serialisation",
                   100.0 * traced / (total_us + json_us));

    std::printf("\n  seed pool: %zu candidates x %zu B = %zu B, built once then copied once\n",
                data.store.count(), sizeof(Candidate), data.store.count() * sizeof(Candidate));
    bench::note("RecallOp ignores that seed (recall_op.hpp:117) — it is a SOURCE node,");
    bench::note("so the pool exists to give the uniform in_count contract a funnel mouth.");
    return 0;
}
