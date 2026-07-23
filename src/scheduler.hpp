#pragma once

#include <memory>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// DagScheduler — runs the operator pipeline and collects the trace.
//
// The Shua Shua cascade is a linear chain (Recall -> Feature -> Score ->
// Rerank), so this executes operators in the order they were added, threading
// each stage's output batch into the next. It is a degenerate DAG: one path, no
// branches.
//
// WHY not a general DAG engine (topological sort, multi-input nodes, explicit
// edges) yet: there is no branching pipeline to schedule — every stage has
// exactly one predecessor. A full DAG engine would be speculative complexity,
// and would raise questions with no answer here (e.g. how to merge two input
// batches into one node), for no operator that needs it. When a branching
// pipeline actually appears, this grows into topological execution; until then,
// sequential is the honest smallest form that satisfies the contract.
// -----------------------------------------------------------------------------
class DagScheduler {
public:
    void add(std::unique_ptr<Operator> op) {
        nodes_.push_back(std::move(op));
    }

    // Execute every node in order over `seed`, appending one TraceEntry per node
    // (each operator records its own via Operator::run). Returns the final batch.
    Batch run(const Batch& seed, std::vector<TraceEntry>& trace) const {
        Batch batch = seed;
        for (const std::unique_ptr<Operator>& node : nodes_) {
            batch = node->run(batch, trace);
        }
        return batch;
    }

private:
    std::vector<std::unique_ptr<Operator>> nodes_;  // pipeline nodes, in run order
};
