#pragma once

#include <string>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// FeatureOp — feature attachment (Stage 2). Target: ~5,000 -> ~5,000.
//
// Job: attach the features the scorer will read to each surviving candidate —
// e.g. category match, recency, popularity. Cardinality is unchanged; this
// stage enriches rather than filters.
//
// OWNER IMPLEMENTS (core): the feature computation, and wherever the computed
// features are carried forward (likely extra columns on Batch — your call).
// -----------------------------------------------------------------------------
class FeatureOp : public Operator {
public:
    std::string name() const override;  // TODO: owner implements
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) override;  // TODO: owner implements
};
