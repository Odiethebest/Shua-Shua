#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "item_store.hpp"
#include "note.hpp"

// -----------------------------------------------------------------------------
// Synthetic data (a FIXTURE, not part of the serving engine).
//
// A production system loads *learned* item embeddings produced offline. This
// project synthesizes them instead, with the one property that makes the demo
// believable: each category has a random centroid vector, and every note in that
// category is `centroid + small noise`. Same-category notes therefore cluster in
// vector space, so similarity recall is genuinely meaningful — a "food" query
// returns food. The content is fabricated; the ranking mechanism is real.
//
// This lives in the engine tree only because M0/M1 need an in-memory store to
// run against; a richer offline generator belongs in scripts/ at a later
// milestone.
// -----------------------------------------------------------------------------
struct SyntheticData {
    ItemStore                       store;      // the built candidate store (SoA)
    std::vector<std::vector<float>> centroids;  // [category][DIM], unit-normalized
};

// Normalize a DIM-length vector in place to unit length.
//
// WHY normalize every stored vector (and every query): with unit vectors the dot
// product IS cosine similarity, so "similar" means "points the same direction"
// (i.e. same category) regardless of magnitude. That keeps the recall kernel a
// plain dot product while making the clustering crisp. Rejected alternative:
// leave vectors un-normalized and rely on raw dot product — it happens to work
// for this data, but ties similarity to vector length, which muddies the demo
// for no benefit.
inline void normalize(float* v, std::size_t dim) {
    float sum_sq = 0.0f;
    for (std::size_t d = 0; d < dim; ++d) {
        sum_sq += v[d] * v[d];
    }
    const float norm = std::sqrt(sum_sq);
    if (norm > 0.0f) {  // guard: never divide a (degenerate) zero vector
        for (std::size_t d = 0; d < dim; ++d) {
            v[d] /= norm;
        }
    }
}

// Build a synthetic store: `categories` clusters of `per_category` notes each.
//
// WHY take an explicit seed and a deterministic PRNG (mt19937): the demo — and
// later the naive-vs-SIMD parity check — must run on identical data every time.
// A fixed seed makes runs reproducible; an unseeded/random source would make
// "result diff = 0" impossible to trust.
inline SyntheticData build_synthetic_data(std::uint8_t categories,
                                          std::uint32_t per_category,
                                          std::uint32_t seed) {
    const std::size_t dim = ItemStore::DIM;

    std::mt19937 rng(seed);
    // Centroid components spread across [-1, 1]; note noise is much smaller so
    // clusters stay separated. WHY these ranges: with DIM=64, independent random
    // centroids are nearly orthogonal, so a noise stddev (~0.1) well below the
    // centroid spread keeps same-category vectors closer to one another than to
    // any other category's centroid — exactly the property recall relies on.
    std::uniform_real_distribution<float> centroid_dist(-1.0f, 1.0f);
    std::normal_distribution<float>       noise_dist(0.0f, 0.1f);
    std::uniform_real_distribution<float> popularity_dist(0.0f, 1.0f);
    // Ages spread flat over a year. WHY uniform: a flat spread makes the recency
    // feature's effect visible across the whole range; the exact age distribution
    // is fixture detail, not engine logic.
    std::uniform_int_distribution<std::uint32_t> age_dist(0, 365);

    SyntheticData data;
    data.centroids.assign(categories, std::vector<float>(dim));

    // One centroid per category, normalized so a query built from it is unit too.
    for (std::uint8_t c = 0; c < categories; ++c) {
        for (std::size_t d = 0; d < dim; ++d) {
            data.centroids[c][d] = centroid_dist(rng);
        }
        normalize(data.centroids[c].data(), dim);
    }

    const std::size_t total = static_cast<std::size_t>(categories) * per_category;
    data.store.embeddings.reserve(total * dim);  // reserve so push_back never reallocates mid-build
    data.store.notes.reserve(total);

    std::uint32_t next_id = 0;
    for (std::uint8_t c = 0; c < categories; ++c) {
        for (std::uint32_t i = 0; i < per_category; ++i) {
            // Append this note's vector: centroid + small noise, then normalize
            // the slice we just wrote.
            for (std::size_t d = 0; d < dim; ++d) {
                data.store.embeddings.push_back(data.centroids[c][d] + noise_dist(rng));
            }
            normalize(&data.store.embeddings[static_cast<std::size_t>(next_id) * dim], dim);

            // WHY id == insertion index: recall returns item indices, and keeping
            // the stored id equal to the index lets us read a note's metadata
            // directly as notes[index] with no id->index map. If ids ever diverge
            // from indices, that mapping has to be reintroduced.
            Note note;
            note.id = next_id;
            note.category = c;
            note.popularity = popularity_dist(rng);
            note.age_days = age_dist(rng);
            data.store.notes.push_back(note);

            ++next_id;
        }
    }

    return data;
}
