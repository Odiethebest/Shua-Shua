#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Batch: the payload that flows between operators.
//
// A batch is the set of candidate item indices (into the ItemStore) the current
// stage is considering. Each operator consumes a batch and returns a (usually
// smaller) one. This is a minimal starting shape — as the cascade grows you will
// likely attach stage-specific columns here (computed features, scores). The
// internal representation is the owner's to evolve; the candidate-id list is the
// one thing every operator shares.
// -----------------------------------------------------------------------------
struct Batch {
    std::vector<std::uint32_t> ids;  // candidate item indices into the ItemStore
};

// -----------------------------------------------------------------------------
// TraceEntry: one operator's self-report. TRACING IS A FIRST-CLASS OUTPUT.
//
// Every operator must emit exactly this record shape; the UI depends on it.
// Do not change the field set without updating the frontend contract.
// -----------------------------------------------------------------------------
struct TraceEntry {
    std::string                name;        // operator name, e.g. "RecallOp"
    std::size_t                in_count;    // candidates received
    std::size_t                out_count;   // candidates emitted
    double                     latency_us;  // wall-clock time spent, microseconds
    std::vector<std::uint32_t> sample_ids;  // a few output ids, for the UI to show
};

// -----------------------------------------------------------------------------
// Operator: the uniform interface every pipeline stage implements.
//
// CONTRACT (see CLAUDE.md): take a batch, return a batch, and record one
// TraceEntry describing what happened. The scheduler owns wiring and execution
// order; an operator only implements its own transform.
//
// This is the interface ONLY. Operator bodies are core engine work — the owner
// implements them (see recall_op.hpp / feature_op.hpp / score_op.hpp /
// rerank_op.hpp). The exact signature below is a suggested shape; refine it as
// you build, but preserve the "batch in -> batch out + one TraceEntry" contract.
// -----------------------------------------------------------------------------
class Operator {
public:
    virtual ~Operator() = default;

    // Stable operator name, used in the trace and the UI.
    virtual std::string name() const = 0;

    // Transform a batch of candidates and append this stage's TraceEntry to
    // `trace`. Returns the (usually smaller) output batch.
    virtual Batch run(const Batch& in, std::vector<TraceEntry>& trace) = 0;
};
