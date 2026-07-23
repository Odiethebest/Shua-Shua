#pragma once

#include <string>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// ScoreOp — multi-objective scoring (Stage 3). Target: ~5,000 -> ~50.
//
// Job: compute a weighted multi-objective score per candidate (e.g. combining
// click / like / save objectives) from the attached features, then keep the
// top scorers.
//
// OWNER IMPLEMENTS (core): the scoring function, its objective weights, and the
// top-k selection.
// -----------------------------------------------------------------------------
class ScoreOp : public Operator {
public:
    std::string name() const override;  // TODO: owner implements
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) override;  // TODO: owner implements
};
