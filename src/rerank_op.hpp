#pragma once

#include <string>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// RerankOp — diversity-aware reranking (Stage 4). Target: ~50 -> ~12.
//
// Job: reorder the top-scored candidates for diversity (an MMR / DPP-style
// tradeoff between relevance and variety) so the final feed is not monotonous,
// then emit the final page.
//
// OWNER IMPLEMENTS (core): the diversity objective and the reordering/selection
// logic.
// -----------------------------------------------------------------------------
class RerankOp : public Operator {
public:
    std::string name() const override;  // TODO: owner implements
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) override;  // TODO: owner implements
};
