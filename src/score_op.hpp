#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// ScoreOp — multi-objective scoring (Stage 3). Target: ~5,000 -> ~50.
//
// Combines the attached features into a single ranking score and keeps the
// top-k highest scorers.
// -----------------------------------------------------------------------------
class ScoreOp : public Operator {
public:
    // Feature weights for the blended score.
    //
    // WHY a transparent weighted sum rather than faked per-objective models: a
    // production ranker predicts several objectives (pCTR, pLike, pSave) with
    // learned models and blends them. There are no learned models here (training
    // is out of scope), so we blend the attached features linearly. The linear
    // form is honest about what it is and has no unexplained magic; fabricated
    // sigmoid "model" curves would add mystery without adding a real model.
    struct Weights {
        float similarity;
        float category_match;
        float recency;
        float popularity;
    };

    ScoreOp(Weights weights, std::size_t k) : weights_(weights), k_(k) {}

    std::string name() const override { return "ScoreOp"; }

protected:
    Batch transform(const Batch& in) const override {
        Batch out = in;
        for (Candidate& c : out.items) {
            c.score = weights_.similarity     * c.similarity
                    + weights_.category_match * c.category_match
                    + weights_.recency        * c.recency
                    + weights_.popularity     * c.popularity;
        }

        // Rank by score, breaking ties by ascending id — same determinism
        // reasoning as recall (a stable, data-defined order).
        std::sort(out.items.begin(), out.items.end(),
                  [](const Candidate& a, const Candidate& b) {
                      if (a.score != b.score) {
                          return a.score > b.score;
                      }
                      return a.id < b.id;
                  });

        if (k_ < out.items.size()) {
            out.items.resize(k_);
        }
        return out;
    }

private:
    Weights     weights_;
    std::size_t k_;  // number of candidates to keep
};
