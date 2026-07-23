#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include "item_store.hpp"
#include "note.hpp"
#include "operator.hpp"

// -----------------------------------------------------------------------------
// RerankOp — diversity-aware reranking (Stage 4). Target: ~50 -> ~12.
//
// Reorders the top-scored candidates for category diversity and emits the final
// page, using greedy MMR (maximal marginal relevance): repeatedly pick the
// candidate with the best marginal value
//
//     value = lambda * score  -  (1 - lambda) * redundancy
//
// where redundancy grows with how many already-picked items share this
// candidate's category. lambda in [0, 1] trades relevance (lambda = 1: pure
// score) against variety (lambda = 0: spread categories).
//
// WHY category overlap as the redundancy signal, instead of vector similarity to
// the already-picked set (classic MMR): the salient "don't be monotonous" axis
// for this feed is category — the goal is not to show twelve food notes in a row.
// Category overlap captures that directly and keeps rerank readable without
// re-entering the dot-product kernel. Vector-similarity MMR is more general but
// buys little here at real cost in complexity.
// -----------------------------------------------------------------------------
class RerankOp : public Operator {
public:
    RerankOp(const ItemStore& store, std::size_t page_size, float lambda)
        : store_(store), page_size_(page_size), lambda_(lambda) {}

    std::string name() const override { return "RerankOp"; }

protected:
    Batch transform(const Batch& in) const override {
        std::vector<Candidate> remaining = in.items;  // candidates not yet placed

        // How many placed items belong to each category so far. WHY size 256:
        // Note::category is a uint8_t, so 256 buckets cover every possible value
        // and we avoid having to be told how many categories exist. The few KB is
        // irrelevant.
        std::vector<std::size_t> category_picks(256, 0);

        const std::size_t page = std::min(page_size_, remaining.size());
        Batch out;
        out.items.reserve(page);

        for (std::size_t placed = 0; placed < page; ++placed) {
            // Pick the remaining candidate with the highest marginal value.
            std::size_t best_index = 0;
            float best_value = -std::numeric_limits<float>::infinity();
            for (std::size_t i = 0; i < remaining.size(); ++i) {
                const Candidate& c = remaining[i];
                const std::uint8_t category = store_.notes[c.id].category;
                const float redundancy = static_cast<float>(category_picks[category]);
                const float value = lambda_ * c.score - (1.0f - lambda_) * redundancy;
                if (value > best_value) {
                    best_value = value;
                    best_index = i;
                }
            }

            const Candidate chosen = remaining[best_index];
            out.items.push_back(chosen);
            category_picks[store_.notes[chosen.id].category] += 1;

            // Remove the chosen candidate from the pool. WHY a plain erase (an
            // O(n) shift) rather than swap-and-pop: at ~50 candidates the shift is
            // free, and erase keeps the code obvious; swap-and-pop would be a
            // micro-optimization that trades clarity for nothing at this scale.
            remaining.erase(remaining.begin() + static_cast<std::ptrdiff_t>(best_index));
        }

        return out;
    }

private:
    const ItemStore& store_;
    std::size_t      page_size_;  // size of the final feed page
    float            lambda_;     // relevance/diversity tradeoff, in [0, 1]
};
