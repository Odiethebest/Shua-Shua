#pragma once

#include <string>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// RecallOp — candidate generation (Stage 1). Target: ~1,000,000 -> ~5,000.
//
// Job: vector-similarity recall. Given a user/query vector, find the most
// similar item vectors in the SoA ItemStore. THIS IS THE HOT PATH.
//
// OWNER IMPLEMENTS (core — off limits for assistants):
//   * the similarity computation over the SoA embeddings;
//   * a naive scalar path AND a SIMD path behind this one interface, kept
//     bit-for-bit identical (see README "Consistency Check");
//   * the parity/diff + speedup reporting. Keep the naive reference path
//     permanently — the zero-diff check is a demo feature.
//
// Wiring note: inject whatever recall needs (a const ItemStore&, the query
// vector, top-k) however you prefer — constructor or a run-context. That is
// yours to design too.
// -----------------------------------------------------------------------------
class RecallOp : public Operator {
public:
    std::string name() const override;  // TODO: owner implements
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) override;  // TODO: owner implements
};
