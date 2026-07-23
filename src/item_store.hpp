#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "note.hpp"

// -----------------------------------------------------------------------------
// ItemStore: the in-memory serving candidate store.
//
// LAYOUT CONTRACT (do NOT refactor to AoS):
//   Item embeddings are held Structure-of-Arrays — ONE flat float buffer with
//   every item vector concatenated, row-major by item — so the similarity
//   kernel streams contiguous memory. Per-item metadata lives in a parallel
//   `notes` array indexed by the same item index.
//
//       embedding for item i  ==  embeddings[i * DIM .. i * DIM + DIM)
//
// Only the *layout* (the fields below) is declared here. All access and
// similarity logic is core engine work (see CLAUDE.md) and is left as TODO for
// the owner. Bodies are intentionally undefined.
// -----------------------------------------------------------------------------
struct ItemStore {
    static constexpr std::size_t DIM = 64;  // fixed embedding dimension

    std::vector<float> embeddings;  // size == count() * DIM, row-major by item (SoA)
    std::vector<Note>  notes;       // per-item metadata, parallel to embeddings

    // Number of items currently in the store.
    std::size_t count() const;  // TODO: owner implements

    // Pointer to the DIM-length embedding for `index` (SoA access).
    const float* vector_of(std::uint32_t index) const;  // TODO: owner implements
};
