#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "item_store.hpp"
#include "note.hpp"
#include "operator.hpp"

// -----------------------------------------------------------------------------
// FeatureOp — feature attachment (Stage 2). Target: ~5,000 -> ~5,000.
//
// Attaches the features the scorer will read to every surviving candidate.
// Cardinality is unchanged: this stage enriches, it does not filter.
// -----------------------------------------------------------------------------
class FeatureOp : public Operator {
public:
    // `category_weights` is the persona's affinity per category (indexed by
    // Note::category). It is the same profile used to build the recall query, so
    // one persona shapes both recall and ranking — as it would in a real system.
    FeatureOp(const ItemStore& store, std::vector<float> category_weights)
        : store_(store), category_weights_(std::move(category_weights)) {}

    std::string name() const override { return "FeatureOp"; }

protected:
    Batch transform(const Batch& in) const override {
        // Copy the candidates through, then fill in the feature columns. WHY copy
        // rather than mutate in place: the interface hands us a const input
        // (every stage's input is immutable), and a feature stage conceptually
        // produces an enriched copy of what it received.
        Batch out = in;
        for (Candidate& c : out.items) {
            const Note& note = store_.notes[c.id];

            // category_match: the user's affinity for this item's category, read
            // straight from the persona weights. WHY from the persona and not
            // recomputed from vectors: "does this match what the user likes" is a
            // profile signal, and the weights already express exactly that.
            c.category_match = category_weights_[note.category];

            // recency: exponential freshness decay from the note's age. WHY
            // exponential (half-life) rather than a linear 1 - age/max: the
            // exponential stays in (0, 1] with no clamping and models "interest
            // fades quickly at first, then levels off" — the usual shape of a
            // freshness feature.
            const float kHalfLifeDays = 30.0f;
            c.recency = std::exp(-static_cast<float>(note.age_days) / kHalfLifeDays);

            // popularity: already normalized to [0, 1] when the store was built,
            // so it passes straight through.
            c.popularity = note.popularity;
        }
        return out;
    }

private:
    const ItemStore&   store_;
    std::vector<float> category_weights_;  // persona affinity per category
};
