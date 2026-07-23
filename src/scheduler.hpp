#pragma once

#include <memory>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// DagScheduler — owns the operator graph and executes it.
//
// Responsibility: hold operators as nodes with dependency edges, execute them in
// topological order (each node's output feeding its successor), and collect the
// per-node TraceEntry sequence the UI animates.
//
// This is a SUGGESTED interface sketch only. Both the exact API and ALL of the
// scheduling / topological-execution logic are core engine work — the owner
// implements them (see CLAUDE.md). Bodies are intentionally left undefined.
// -----------------------------------------------------------------------------
class DagScheduler {
public:
    // Add an operator node to the graph. (The edge/wiring API is the owner's to
    // design — insertion order for a linear cascade, or explicit dependencies
    // for a true DAG.)
    void add(std::unique_ptr<Operator> op);  // TODO: owner implements

    // Execute the graph over a seed batch and return the final batch, appending
    // one TraceEntry per node in execution order.
    Batch run(const Batch& seed, std::vector<TraceEntry>& trace);  // TODO: owner implements

private:
    std::vector<std::unique_ptr<Operator>> nodes_;  // graph nodes (wiring: owner)
};
