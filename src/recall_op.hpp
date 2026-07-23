#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "item_store.hpp"
#include "operator.hpp"

// -----------------------------------------------------------------------------
// RecallOp — candidate generation (Stage 1). Target: ~1,000,000 -> ~5,000.
//
// Design across milestones (why this file holds both a function and a class):
//   * M0 implemented the recall *computation* as recall_naive() below — a plain
//     scalar reference and the permanent baseline.
//   * M1 wraps it behind the RecallOp Operator interface (transform + trace).
//   * M2 will add recall_simd() with the SAME signature and assert it returns
//     output identical to recall_naive() (the parity diff), then report speedup.
// Keeping the naive function standalone is what makes that later diff trivial:
// two functions, one comparison.
// -----------------------------------------------------------------------------

// One recall result: which item, and how similar it was to the query.
struct Scored {
    std::uint32_t id;     // item index into the ItemStore
    float         score;  // dot-product similarity to the query vector
};

// Naive scalar recall — the permanent reference implementation.
//
// Scans the whole store, scores every item by dot product against `query`, and
// returns the top `k` ranked best-first.
//
// WHY a full linear scan: recall's job is to prune a huge pool cheaply, and the
// production answer to "don't scan everything" is an ANN index (HNSW) — a stretch
// goal, deliberately out of scope. At M0/M1 the store is a few thousand items in
// memory, so a straight scan is both fast enough and the clearest reference for
// the SIMD version to reproduce later.
inline std::vector<Scored> recall_naive(const ItemStore& store,
                                        const float* query,
                                        std::size_t k) {
    const std::size_t dim = ItemStore::DIM;
    const std::size_t n = store.count();

    std::vector<Scored> scored;
    scored.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* vec = store.vector_of(static_cast<std::uint32_t>(i));

        // Dot product of the query with this item's vector. This is the one hot
        // computation of the whole engine and the exact loop M2 vectorizes,
        // written here as the plainest scalar accumulate so the SIMD version has
        // an unambiguous reference to match.
        float dot = 0.0f;
        for (std::size_t d = 0; d < dim; ++d) {
            dot += query[d] * vec[d];
        }

        scored.push_back(Scored{static_cast<std::uint32_t>(i), dot});
    }

    // Rank best-first, breaking ties by ascending id.
    //
    // WHY a deterministic tie-break: in M2 the SIMD path accumulates in a
    // different order and can produce a fractionally different sum; if two items
    // tie, an unspecified sort could order them differently between the two paths
    // and break the parity diff for no real reason. Ordering ties by id removes
    // that ambiguity so "identical output" is a meaningful bar.
    //
    // WHY a full std::sort instead of std::partial_sort / a heap for top-k: at
    // this store size the cost difference is negligible, and a full sort with an
    // explicit comparator is the most obviously-correct thing to read. Switching
    // to partial_sort is a fair optimization once n grows large — premature now.
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) {
            return a.score > b.score;  // higher similarity first
        }
        return a.id < b.id;  // deterministic tie-break
    });

    if (k < scored.size()) {
        scored.resize(k);
    }
    return scored;
}

// -----------------------------------------------------------------------------
// RecallOp — the Operator wrapper around recall_naive.
// -----------------------------------------------------------------------------
class RecallOp : public Operator {
public:
    // WHY own a copy of the query (std::vector) instead of borrowing a const
    // float*: the operator then does not depend on the caller keeping a buffer
    // alive for the operator's whole lifetime. The query is DIM floats — a
    // trivially cheap copy.
    RecallOp(const ItemStore& store, std::vector<float> query, std::size_t k)
        : store_(store), query_(std::move(query)), k_(k) {}

    std::string name() const override { return "RecallOp"; }

protected:
    // WHY ignore the input batch: recall is the SOURCE of candidates. It streams
    // the store's contiguous SoA buffer directly (that contiguity is what makes
    // M2's SIMD kernel pay off) rather than walking an input id list, which would
    // be a scattered gather. The scheduler seeds the pipeline with the full pool,
    // so the trace still reports in_count = pool size for this stage.
    Batch transform(const Batch& /*in*/) const override {
        const std::vector<Scored> top = recall_naive(store_, query_.data(), k_);

        Batch out;
        out.items.reserve(top.size());
        for (const Scored& s : top) {
            Candidate c;
            c.id = s.id;
            c.similarity = s.score;
            out.items.push_back(c);
        }
        return out;
    }

private:
    const ItemStore&   store_;
    std::vector<float> query_;  // owned, unit-normalized query vector
    std::size_t        k_;      // number of candidates to recall
};
