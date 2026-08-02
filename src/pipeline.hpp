#pragma once

#include <memory>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// Pipeline — runs the operator chain and collects the trace.
//
// Executes operators in the order they were added, threading each stage's output
// batch into the next.
//
// NAMING. This class was called DagScheduler. It was renamed because the old name
// promised more than the code delivers: a reader sees "DAG" and expects declared
// edges and a scheduling decision, and finds a for-loop. A name that oversells is
// a defect of its own — it misleads before anyone reaches the body. What is here
// is a sequential chain, so it is now called what it is.
//
// WHAT WOULD ACTUALLY MAKE THIS A DAG — and it is not a topological sort. That is
// the piece people reach for first, but on a straight line it is a no-op: the
// topological order of a chain is the chain. Ordering only becomes a question when
// there is structure to order:
//
//   1. BRANCHING (fan-out) — one node feeding two or more successors, e.g. several
//      recall routes issued from the same query.
//   2. FAN-IN (multi-input nodes) — a node consuming two or more predecessors, e.g.
//      a merge/dedup node unioning those routes. This is the hard one, and the real
//      reason topological sort alone buys nothing here: the moment a node has two
//      inputs, run() can no longer thread a single Batch through, and the operator
//      contract itself has to grow a rule for how multiple input batches combine.
//   3. THEN dependency declaration, topological order, and cycle detection — to
//      choose a legal execution order and reject an illegal graph.
//
// So the missing piece is not the scheduling algorithm; it is a pipeline whose
// shape needs scheduling. Until one exists, a general graph engine is speculative
// complexity that also forces answers to questions nothing here asks (how do two
// input batches merge? what if one branch comes back empty?).
//
// WHY THIS IS STILL A CLASS rather than five calls inlined into api.hpp:
//   * Operators are pluggable — adding one changes no code in this file.
//   * It is the single point where every stage's trace is collected, so the trace
//     shape cannot drift from stage to stage.
// -----------------------------------------------------------------------------
class Pipeline {
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
    std::vector<std::unique_ptr<Operator>> nodes_;  // chain nodes, in run order
};
