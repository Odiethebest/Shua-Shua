// -----------------------------------------------------------------------------
// Shua Shua — native kernel (Milestone M0).
//
// M0 scope: build a synthetic in-memory store whose vectors cluster by category,
// run one naive similarity recall for a chosen "persona", and print the top
// matches. This is the recall computation only — the Operator/DAG/trace
// machinery arrives in M1, and the SIMD kernel + parity diff in M2.
//
// Build:
//   clang++ -std=c++20 -O2 src/main.cpp -o shuashua && ./shuashua
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "item_store.hpp"
#include "note.hpp"
#include "recall_op.hpp"
#include "synthetic.hpp"

namespace {

// Human-readable category labels, indexed by Note::category. Presentation only —
// the engine only ever sees the numeric category.
const char* const CATEGORY_NAMES[] = {
    "food", "fashion", "travel", "tech", "fitness", "beauty",
};
constexpr std::uint8_t NUM_CATEGORIES =
    static_cast<std::uint8_t>(sizeof(CATEGORY_NAMES) / sizeof(CATEGORY_NAMES[0]));

}  // namespace

int main() {
    // Demo parameters. Small enough to print, large enough that clustering is
    // obvious. Fixed seed => reproducible output (and, later, a reproducible
    // parity check in M2).
    const std::uint32_t per_category = 500;
    const std::uint32_t seed = 42;
    const std::size_t   top_n = 10;
    const std::uint8_t  persona = 0;  // category to query for: "food"

    const SyntheticData data = build_synthetic_data(NUM_CATEGORIES, per_category, seed);
    const ItemStore& store = data.store;

    // The persona query is that category's centroid: "show me things like the
    // centre of the food cluster." Every top result should therefore be food —
    // that is the M0 success criterion (recall is semantically meaningful).
    const std::vector<float>& query = data.centroids[persona];

    const std::vector<Scored> top = recall_naive(store, query.data(), top_n);

    std::cout << "Shua Shua - M0 kernel: naive similarity recall\n";
    std::cout << "Store: " << store.count() << " notes, "
              << static_cast<int>(NUM_CATEGORIES) << " categories, DIM="
              << ItemStore::DIM << "\n";
    std::cout << "Persona query: category \"" << CATEGORY_NAMES[persona] << "\"\n";
    std::cout << "Top " << top_n << " by similarity:\n";

    for (std::size_t rank = 0; rank < top.size(); ++rank) {
        const Scored& s = top[rank];
        const Note& note = store.notes[s.id];
        std::cout << "  #" << (rank + 1)
                  << "  id=" << s.id
                  << "  cat=" << CATEGORY_NAMES[note.category]
                  << "  sim=" << s.score << "\n";
    }

    return 0;
}
