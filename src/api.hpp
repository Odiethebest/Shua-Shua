#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "feature_op.hpp"
#include "item_store.hpp"
#include "mix_op.hpp"
#include "note.hpp"
#include "operator.hpp"
#include "recall_op.hpp"
#include "rerank_op.hpp"
#include "scheduler.hpp"
#include "score_op.hpp"
#include "synthetic.hpp"

// -----------------------------------------------------------------------------
// api.hpp — the engine's single entry point and its JSON boundary.
//
// This is the presentation/glue layer, NOT engine algorithm: it builds the
// resident store once, assembles the operator cascade for a chosen persona, runs
// it, and serializes the feed + trace to JSON. The WASM build (M3, Part B) binds
// `recommend`/`to_json` straight through to JS via embind; the native build calls
// the same functions from main.cpp. One orchestration, two front doors.
// -----------------------------------------------------------------------------

// Human-readable category labels, indexed by Note::category. Presentation only.
constexpr const char* CATEGORY_NAMES[] = {
    "food", "fashion", "travel", "tech", "fitness", "beauty",
};
constexpr std::uint8_t NUM_CATEGORIES =
    static_cast<std::uint8_t>(sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]));

inline std::string category_name(std::uint8_t category) {
    return category < NUM_CATEGORIES ? CATEGORY_NAMES[category] : "unknown";
}

// Store + pipeline configuration, in one place so the native driver and the WASM
// boundary agree.
constexpr std::uint32_t kPerCategory = 500;   // notes per category
constexpr std::uint32_t kSeed = 42;           // fixed => reproducible
constexpr std::size_t   kRecallK = 300;       // recall keeps this many candidates
constexpr std::size_t   kScoreK = 50;         // score keeps this many
constexpr std::size_t   kPageSize = 12;       // final feed size
constexpr float         kRerankLambda = 0.7f; // relevance/diversity tradeoff
constexpr std::size_t   kRerankPool = 24;     // profile path: diverse pool MixOp draws from (> page)
constexpr std::size_t   kExploreFloor = 2;    // profile path: guaranteed exploration slots in the page

// A named persona for the UI's persona switcher: a label plus per-category
// interest weights (indexed by Note::category).
struct Persona {
    std::string        label;
    std::vector<float> category_weights;
};

// Small helper: build a category-weight vector from (category, weight) pairs, the
// rest zero.
inline std::vector<float> weights_for(
    std::initializer_list<std::pair<std::uint8_t, float>> pairs) {
    std::vector<float> w(NUM_CATEGORIES, 0.0f);
    for (const auto& [category, weight] : pairs) {
        w[category] = weight;
    }
    return w;
}

// The demo personas the UI can switch between. (Config, not engine.)
inline const std::vector<Persona>& personas() {
    static const std::vector<Persona> list = {
        {"Foodie", weights_for({{0, 1.0f}})},
        {"Traveler", weights_for({{2, 1.0f}})},
        {"Foodie + Traveler", weights_for({{0, 0.5f}, {2, 0.5f}})},
        {"Techie", weights_for({{3, 1.0f}})},
    };
    return list;
}

// The resident item store — built ONCE on first use and reused for every request.
//
// WHY a singleton: a serving engine keeps its candidate store resident in memory
// for microsecond access; rebuilding 3000 vectors on every request (and every
// WASM call from JS) would defeat the entire point. A function-local static is
// built exactly once, lazily, on the first recommend().
inline const SyntheticData& shared_data() {
    static const SyntheticData data = build_synthetic_data(NUM_CATEGORIES, kPerCategory, kSeed);
    return data;
}

// Build a query vector from a persona: the weighted sum of category centroids,
// unit-normalized so it is directly comparable to the (also unit) item vectors.
inline std::vector<float> make_query(const std::vector<float>& category_weights,
                                     const std::vector<std::vector<float>>& centroids) {
    std::vector<float> query(ItemStore::DIM, 0.0f);
    for (std::size_t c = 0; c < category_weights.size(); ++c) {
        const float w = category_weights[c];
        for (std::size_t d = 0; d < ItemStore::DIM; ++d) {
            query[d] += w * centroids[c][d];
        }
    }
    normalize(query.data(), ItemStore::DIM);
    return query;
}

// Seed batch = every item in the store: the candidate pool recall draws from. Its
// size is what the trace reports as RecallOp's in_count (the top of the funnel).
inline Batch full_pool(const ItemStore& store) {
    Batch pool;
    pool.items.reserve(store.count());
    for (std::size_t i = 0; i < store.count(); ++i) {
        Candidate c;
        c.id = static_cast<std::uint32_t>(i);
        pool.items.push_back(c);
    }
    return pool;
}

// The result of one recommend() call: the persona label, the final feed, and the
// per-operator trace.
struct Recommendation {
    std::string             persona_label;
    Batch                   feed;
    std::vector<TraceEntry> trace;
};

// Run the full cascade for a given query vector + per-category weights, and
// return the feed + trace. Shared by recommend() (a persona's blended query) and
// recommend_similar() (an item's own vector as the query) — the ONLY thing that
// differs between them is where the query comes from.
inline Recommendation run_recommendation(std::vector<float> query,
                                         std::vector<float> category_weights,
                                         std::string label,
                                         std::vector<std::uint32_t> seen_ids = {},
                                         int new_ratio = 100,
                                         std::size_t explore_floor = 0) {
    const SyntheticData& data = shared_data();

    DagScheduler pipeline;
    pipeline.add(std::make_unique<RecallOp>(data.store, std::move(query), kRecallK,
                                            RecallKernel::Simd));
    pipeline.add(std::make_unique<FeatureOp>(data.store, std::move(category_weights)));
    pipeline.add(std::make_unique<ScoreOp>(
        ScoreOp::Weights{/*similarity=*/1.0f, /*category_match=*/0.5f,
                         /*recency=*/0.3f, /*popularity=*/0.2f},
        kScoreK));

    // Final stage(s). The persona/item paths rank straight to the page (RerankOp ->
    // kPageSize). The profile path adds MixOp after a wider diverse rerank pool:
    // "mostly new + a few strong seen" (new_ratio) PLUS a guaranteed exploration
    // floor drawn from outside the dominant category. The mix runs whenever there is
    // an exploration floor to enforce or a seen set to balance (so the profile path,
    // which always passes explore_floor > 0, always mixes — even a first, unclicked
    // feed still gets its exploration slots).
    const bool use_mix = explore_floor > 0 || (!seen_ids.empty() && new_ratio < 100);
    if (use_mix) {
        pipeline.add(std::make_unique<RerankOp>(data.store, kRerankPool, kRerankLambda));
        pipeline.add(std::make_unique<MixOp>(data.store, std::move(seen_ids), kPageSize,
                                             new_ratio, explore_floor));
    } else {
        pipeline.add(std::make_unique<RerankOp>(data.store, kPageSize, kRerankLambda));
    }

    Recommendation rec;
    rec.persona_label = std::move(label);
    rec.feed = pipeline.run(full_pool(data.store), rec.trace);
    return rec;
}

// THE entry point: run the full cascade for a persona and return the feed +
// trace. This is what the WASM boundary exposes to JS.
inline Recommendation recommend(int persona_id) {
    // Clamp to a valid persona. WHY clamp instead of throw: this is called across
    // the JS/WASM boundary, where an out-of-range index should degrade gracefully
    // rather than throw an exception through the FFI.
    const std::size_t count = personas().size();
    const std::size_t idx =
        (persona_id >= 0 && static_cast<std::size_t>(persona_id) < count)
            ? static_cast<std::size_t>(persona_id)
            : 0;
    const Persona& persona = personas()[idx];

    // A persona's query is the weighted blend of its categories' centroids.
    return run_recommendation(make_query(persona.category_weights, shared_data().centroids),
                              persona.category_weights, persona.label);
}

// Item-based recall: use a clicked item's OWN vector as the query, so the feed
// shifts toward that item's neighborhood ("show me more like this"). Same
// pipeline as recommend(); only the query source differs. Models the implicit
// signal "user clicked X → surface items similar to X".
inline Recommendation recommend_similar(int item_id) {
    const SyntheticData& data = shared_data();
    const std::size_t n = data.store.count();

    // Out-of-range id degrades gracefully (same reasoning as recommend's clamp).
    if (item_id < 0 || static_cast<std::size_t>(item_id) >= n) {
        return recommend(0);
    }
    const std::uint32_t id = static_cast<std::uint32_t>(item_id);

    // Query = the item's own (already unit-normalized) embedding. WHY copy into a
    // vector: RecallOp owns its query, whereas vector_of returns a pointer into
    // the store's live buffer that we should not hand off for the op's lifetime.
    const float* vec = data.store.vector_of(id);
    std::vector<float> query(vec, vec + ItemStore::DIM);

    // Reward the clicked item's own category — the implicit "you like this kind
    // of thing" signal that shapes FeatureOp's category_match.
    std::vector<float> weights(NUM_CATEGORIES, 0.0f);
    const std::uint8_t cat = data.store.notes[id].category;
    if (cat < NUM_CATEGORIES) {
        weights[cat] = 1.0f;
    }

    return run_recommendation(std::move(query), std::move(weights),
                              "Similar to #" + std::to_string(id));
}

// Profile-based recall (v2 · B5): the query is built from the user's live, decaying
// profile instead of a fixed persona. The frontend collapses its tag weights into
// per-category weights (indexed exactly like CATEGORY_NAMES) and passes them here;
// the vector-space math stays in C++ (make_query), so the profile query is built the
// SAME way as a persona query and the two cannot drift. This is the v2 "profile
// vector = recall query" entry point; only the source of the weights differs.
inline Recommendation recommend_from_profile(std::vector<float> category_weights,
                                             std::vector<std::uint32_t> seen_ids = {},
                                             int new_ratio = 100) {
    // Guard (mirrors the frontend's §6 neutral fallback): a wrong-sized or all-zero
    // weight vector would make a zero/NaN query. Fall back to a uniform blend so the
    // feed is a diverse sampler rather than empty/degenerate.
    if (category_weights.size() != NUM_CATEGORIES) {
        category_weights.assign(NUM_CATEGORIES, 1.0f);
    }
    float total = 0.0f;
    for (float w : category_weights) total += (w > 0.0f ? w : 0.0f);
    if (total <= 0.0f) {
        category_weights.assign(NUM_CATEGORIES, 1.0f);
    }

    return run_recommendation(make_query(category_weights, shared_data().centroids),
                              category_weights, "For you", std::move(seen_ids), new_ratio,
                              kExploreFloor);
}

// Escape a string for embedding in JSON (only the two characters our data could
// ever contain; category names and persona labels are plain ASCII).
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

// Serialize a Recommendation to a JSON string: { persona, feed[], trace[] }. This
// is the exact payload the React UI (M4) will consume. Hand-written on purpose —
// the shape is small and fixed, so a JSON dependency would not earn its keep.
inline std::string to_json(const Recommendation& rec) {
    const ItemStore& store = shared_data().store;
    std::ostringstream os;

    os << "{\"persona\":\"" << json_escape(rec.persona_label) << "\",\"feed\":[";
    for (std::size_t i = 0; i < rec.feed.items.size(); ++i) {
        const Candidate& c = rec.feed.items[i];
        const Note& note = store.notes[c.id];
        if (i != 0) os << ",";
        os << "{\"id\":" << c.id
           << ",\"category\":\"" << json_escape(category_name(note.category)) << "\""
           << ",\"score\":" << c.score
           << ",\"similarity\":" << c.similarity
           << ",\"category_match\":" << c.category_match
           << ",\"recency\":" << c.recency
           << ",\"popularity\":" << c.popularity << "}";
    }

    os << "],\"trace\":[";
    for (std::size_t i = 0; i < rec.trace.size(); ++i) {
        const TraceEntry& e = rec.trace[i];
        if (i != 0) os << ",";
        os << "{\"name\":\"" << json_escape(e.name) << "\""
           << ",\"in\":" << e.in_count
           << ",\"out\":" << e.out_count
           << ",\"latency_us\":" << e.latency_us
           << ",\"sample_ids\":[";
        for (std::size_t j = 0; j < e.sample_ids.size(); ++j) {
            if (j != 0) os << ",";
            os << e.sample_ids[j];
        }
        os << "],\"detail\":\"" << json_escape(e.detail) << "\"}";
    }
    os << "]}";
    return os.str();
}
