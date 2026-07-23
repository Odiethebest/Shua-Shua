// -----------------------------------------------------------------------------
// Shua Shua — native kernel (Milestone M1).
//
// M1 scope: wire the four operators (Recall -> Feature -> Score -> Rerank) into
// the DagScheduler, run them over a synthetic store for a blended persona, and
// print both the final feed and the per-operator DAG trace. The SIMD recall
// kernel + naive/simd parity diff arrive in M2.
//
// Build:
//   clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

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

}  // namespace

int main() {
    constexpr std::uint32_t per_category = 500;
    constexpr std::uint32_t seed = 42;

    const SyntheticData data = build_synthetic_data(NUM_CATEGORIES, per_category, seed);
    const ItemStore& store = data.store;
    const Persona persona = make_persona(data);

    // Build the cascade. Cardinalities shrink stage by stage: pool -> recall ->
    // (features attached, count unchanged) -> score top-k -> final page.
    DagScheduler pipeline;
    pipeline.add(std::make_unique<RecallOp>(store, persona.query, /*k=*/300));
    pipeline.add(std::make_unique<FeatureOp>(store, persona.category_weights));
    pipeline.add(std::make_unique<ScoreOp>(
        ScoreOp::Weights{/*similarity=*/1.0f, /*category_match=*/0.5f,
                         /*recency=*/0.3f, /*popularity=*/0.2f},
        /*k=*/50));
    pipeline.add(std::make_unique<RerankOp>(store, /*page_size=*/12, /*lambda=*/0.7f));

    std::vector<TraceEntry> trace;
    const Batch feed = pipeline.run(full_pool(store), trace);

    std::cout << "Shua Shua - M1 DAG: Recall -> Feature -> Score -> Rerank\n";
    std::cout << "Store: " << store.count() << " notes, "
              << static_cast<int>(NUM_CATEGORIES) << " categories, DIM="
              << ItemStore::DIM << "\n";
    std::cout << "Persona: 50% food + 50% travel\n";

    print_feed(feed, store);
    print_trace(trace);
    return 0;
}
