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
// -----------------------------------------------------------------------------
struct ItemStore {
    static constexpr std::size_t DIM = 64;  // fixed embedding dimension

    std::vector<float> embeddings;  // size == count() * DIM, row-major by item (SoA)
    std::vector<Note>  notes;       // per-item metadata, parallel to embeddings

    // Number of items in the store.
    //
    // WHY derive this from the embedding buffer rather than notes.size(): the
    // flat buffer is the authoritative store of vectors and count()*DIM is its
    // length by construction. `notes` is a parallel array built in lockstep;
    // treating embeddings as the single source of truth means "how many vectors
    // do I have" has one answer that cannot drift from the other.
    std::size_t count() const {
        return embeddings.size() / DIM;
    }

    // Pointer to the DIM contiguous floats that make up item `index`'s vector.
    //
    // WHY return a raw const pointer instead of copying or wrapping in a span:
    // the entire point of the SoA layout is that the recall kernel can walk item
    // vectors with zero copying and cache-friendly strides — a pointer into the
    // existing buffer gives exactly that. A copy would defeat the layout; a span
    // would be equally cheap but adds a type the plain scalar loop does not need
    // yet. We widen `index` to size_t before multiplying so the byte offset is
    // computed in 64-bit and cannot overflow for a large store.
    const float* vector_of(std::uint32_t index) const {
        return embeddings.data() + static_cast<std::size_t>(index) * DIM;
    }
};
