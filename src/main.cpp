// -----------------------------------------------------------------------------
// Shua Shua — native kernel entry point (Milestone M0).
//
// This is a PLACEHOLDER. It confirms the toolchain works and that the engine
// skeleton headers parse under C++20. The real M0 kernel — build synthetic
// notes, run recall over the SoA store, print the top matches — is core engine
// work for the repository owner to implement (see CLAUDE.md). This file
// intentionally constructs and runs no operator yet.
//
// Build:
//   clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
// -----------------------------------------------------------------------------

#include <iostream>

// Pull in the engine skeleton so a single-file build verifies it all parses.
#include "note.hpp"
#include "item_store.hpp"
#include "operator.hpp"
#include "recall_op.hpp"
#include "feature_op.hpp"
#include "score_op.hpp"
#include "rerank_op.hpp"
#include "scheduler.hpp"

int main() {
    std::cout << "Shua Shua - recommendation serving engine (M0 kernel skeleton)\n";
    std::cout << "Pipeline (DAG): RecallOp -> FeatureOp -> ScoreOp -> RerankOp\n";
    std::cout << "SoA ItemStore embedding dim: " << ItemStore::DIM << "\n";
    std::cout << "Engine not yet implemented - scaffolding only; the owner writes the core.\n";
    return 0;
}
