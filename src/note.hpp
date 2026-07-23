#pragma once

#include <cstdint>

// -----------------------------------------------------------------------------
// Note: one item ("note") in the candidate store.
//
// This is plain item metadata only — the scalar signals the ranking cascade
// reads (category, popularity, ...). The high-dimensional embedding used by
// recall is deliberately NOT stored here; it lives column-wise in the SoA
// ItemStore (see item_store.hpp) so the similarity kernel can stream contiguous
// memory. Presentation-only fields (title, cover image) are also kept out of
// this hot struct and belong in a parallel array on the UI side.
// -----------------------------------------------------------------------------
struct Note {
    std::uint32_t id;          // stable item id
    std::uint8_t  category;    // food / fashion / travel / tech / ...
    float         popularity;  // like count, normalized to [0, 1]
};
