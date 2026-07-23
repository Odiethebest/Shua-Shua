#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "dot.hpp"
#include "item_store.hpp"
#include "operator.hpp"

// -----------------------------------------------------------------------------
// RecallOp — candidate generation (Stage 1). Target: ~1,000,000 -> ~5,000.
//
// Recall = score every candidate by similarity (the hot dot-product scan), then
// keep the top-k. The scan is the part M2 accelerates with SIMD; the ranking is
// shared. Both kernels are exposed behind recall_naive / recall_simd, and the
// parity check (see main.cpp) proves they agree.
// -----------------------------------------------------------------------------

// One recall result: which item, and how similar it was to the query.
struct Scored {
    std::uint32_t id;     // item index into the ItemStore
    float         score;  // dot-product similarity to the query vector
};

// Which inner-product kernel recall uses. Both produce the same ranking; SIMD is
// the faster path (proven by the M2 parity + speedup check).
enum class RecallKernel { Scalar, Simd };

// Score every item in the store against the query using `dot`. Scan only, no
// ranking.
//
// WHY a full linear scan: recall's job is to prune a huge pool cheaply, and the
// production answer to "don't scan everything" is an ANN index (HNSW) — a stretch
// goal, deliberately out of scope. At this store size a straight scan is fast
// enough and is the clearest reference. This scan is exactly what SIMD speeds up.
inline std::vector<Scored> score_all(const ItemStore& store, const float* query,
                                     float (*dot)(const float*, const float*, std::size_t)) {
    const std::size_t n = store.count();
    std::vector<Scored> scored;
    scored.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float* vec = store.vector_of(static_cast<std::uint32_t>(i));
        scored.push_back(Scored{static_cast<std::uint32_t>(i), dot(query, vec, ItemStore::DIM)});
    }
    return scored;
}

// Rank scored candidates best-first (ties by ascending id), then keep the top k.
//
// WHY a deterministic tie-break: the SIMD path accumulates in a different order
// and can produce a fractionally different sum; if two items tie, an unspecified
// sort could order them differently between the two paths and break the parity
// diff for no real reason. Ordering ties by id removes that ambiguity.
//
// WHY a full std::sort rather than std::partial_sort / a heap for top-k: at this
// store size the cost difference is negligible, and a full sort with an explicit
// comparator is the most obviously-correct thing to read. partial_sort is a fair
// optimization once n grows large — premature now.
inline void rank_topk(std::vector<Scored>& scored, std::size_t k) {
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) {
            return a.score > b.score;  // higher similarity first
        }
        return a.id < b.id;  // deterministic tie-break
    });
    if (k < scored.size()) {
        scored.resize(k);
    }
}

// Recall = scan (score_all) + rank (rank_topk).
//
// WHY route both kernels through this one function: it makes the naive/SIMD
// comparison isolate EXACTLY the dot-product kernel. The scan differs (scalar vs
// NEON); the ranking is shared, so it cannot drift between the two and pollute
// the parity result. Difference in output, if any, comes only from the kernel.
inline std::vector<Scored> recall_with(const ItemStore& store, const float* query, std::size_t k,
                                       float (*dot)(const float*, const float*, std::size_t)) {
    std::vector<Scored> scored = score_all(store, query, dot);
    rank_topk(scored, k);
    return scored;
}

// The two public recall entry points. recall_naive is the PERMANENT reference —
// do not delete it once SIMD works; the zero-diff check depends on it.
inline std::vector<Scored> recall_naive(const ItemStore& store, const float* query, std::size_t k) {
    return recall_with(store, query, k, dot_scalar);
}
inline std::vector<Scored> recall_simd(const ItemStore& store, const float* query, std::size_t k) {
    return recall_with(store, query, k, dot_simd);
}

// -----------------------------------------------------------------------------
// RecallOp — the Operator wrapper. Picks a kernel; defaults to the fast one.
// -----------------------------------------------------------------------------
class RecallOp : public Operator {
public:
    // WHY own a copy of the query (std::vector) instead of borrowing a const
    // float*: the operator then does not depend on the caller keeping a buffer
    // alive for its whole lifetime. The query is DIM floats — a cheap copy.
    RecallOp(const ItemStore& store, std::vector<float> query, std::size_t k,
             RecallKernel kernel = RecallKernel::Simd)
        : store_(store), query_(std::move(query)), k_(k), kernel_(kernel) {}

    std::string name() const override { return "RecallOp"; }

protected:
    // WHY ignore the input batch: recall is the SOURCE of candidates. It streams
    // the store's contiguous SoA buffer directly (that contiguity is what makes
    // the SIMD kernel pay off) rather than walking an input id list, which would
    // be a scattered gather. The scheduler seeds the pipeline with the full pool,
    // so the trace still reports in_count = pool size for this stage.
    Batch transform(const Batch& /*in*/) const override {
        std::vector<Scored> top;
        if (kernel_ == RecallKernel::Simd) {
            top = recall_simd(store_, query_.data(), k_);
        } else {
            top = recall_naive(store_, query_.data(), k_);
        }

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
    std::vector<float> query_;   // owned, unit-normalized query vector
    std::size_t        k_;       // number of candidates to recall
    RecallKernel       kernel_;  // which dot-product kernel to use
};
