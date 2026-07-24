#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "operator.hpp"

// -----------------------------------------------------------------------------
// MixOp — the new/seen mix (Stage 5, profile path only). Target: ~24 -> 12.
//
// Assembles the final page as "mostly new + a few strongly-related seen": the
// EXPLORATION / EXPLOITATION tradeoff in miniature. Explore = surface items the
// user has not seen; exploit = keep a small quota of already-seen items that still
// score highly (proven favorites). Without it, a refresh would just replay the
// items the user already clicked (those score highest precisely because they were
// clicked), so the feed would never move on.
//
// WHY a distinct final stage rather than folding this into RerankOp: RerankOp's job
// is category diversity (MMR); this stage's job is recency-of-exposure balance.
// One purpose per operator keeps each readable, and it lets the DAG trace show the
// mix as its own step — the trace is the product. Placed LAST so the new/seen quota
// is guaranteed in the emitted page; a mix-before-rerank order would let RerankOp's
// diversity cut drop the reserved seen items.
//
// new_ratio is the target percentage (0..100) of the page that should be NEW; the
// rest is the seen quota. If either pool is short, the page backfills from the
// other (preferring new) so it is never under-filled.
// -----------------------------------------------------------------------------
class MixOp : public Operator {
public:
    MixOp(std::vector<std::uint32_t> seen_ids, std::size_t page_size, int new_ratio)
        : seen_(seen_ids.begin(), seen_ids.end()),
          page_size_(page_size),
          new_ratio_(new_ratio) {}

    std::string name() const override { return "MixOp"; }

protected:
    Batch transform(const Batch& in) const override {
        // Split the ranked input into new vs. seen, preserving rank order.
        std::vector<Candidate> new_items;
        std::vector<Candidate> seen_items;
        for (const Candidate& c : in.items) {
            if (seen_.count(c.id) != 0) {
                seen_items.push_back(c);
            } else {
                new_items.push_back(c);
            }
        }

        // Target counts: new_target is a share of the page, the rest is the seen
        // quota. Clamp to what each pool can supply, then backfill any shortfall
        // from the other pool (prefer more new — explore) so the page fills.
        std::size_t new_target = static_cast<std::size_t>(
            std::llround(static_cast<double>(page_size_) * new_ratio_ / 100.0));
        if (new_target > page_size_) new_target = page_size_;

        std::size_t take_new = std::min(new_target, new_items.size());
        std::size_t take_seen = std::min(page_size_ - new_target, seen_items.size());

        std::size_t shortfall = page_size_ - take_new - take_seen;
        if (shortfall > 0) {
            const std::size_t extra_new = std::min(shortfall, new_items.size() - take_new);
            take_new += extra_new;
            shortfall -= extra_new;
        }
        if (shortfall > 0) {
            const std::size_t extra_seen = std::min(shortfall, seen_items.size() - take_seen);
            take_seen += extra_seen;
        }

        // Emit new items first (fresh content up top), then the reserved seen.
        Batch out;
        out.items.reserve(take_new + take_seen);
        for (std::size_t i = 0; i < take_new; ++i) out.items.push_back(new_items[i]);
        for (std::size_t i = 0; i < take_seen; ++i) out.items.push_back(seen_items[i]);
        return out;
    }

private:
    std::unordered_set<std::uint32_t> seen_;       // ids the user has already clicked
    std::size_t                       page_size_;  // final feed size
    int                               new_ratio_;  // target % of the page that is NEW
};
