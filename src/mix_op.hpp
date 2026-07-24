#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "item_store.hpp"
#include "note.hpp"
#include "operator.hpp"

// -----------------------------------------------------------------------------
// MixOp — the new/seen mix + exploration floor (Stage 5, profile path only).
// Target: ~24 -> 12.
//
// Assembles the final page in two parts:
//   * EXPLOIT (page_size - explore_floor slots): ranked candidates, "mostly new + a
//     few strongly-related seen" (the new/seen split from new_ratio). This is where
//     a strong preference legitimately dominates — over-personalization is CORRECT
//     behavior for a clear preference, NOT a bug to remove.
//   * EXPLORE (explore_floor slots, guaranteed): items sampled from OUTSIDE the
//     dominant category, so the page is never 100% one thing no matter how skewed
//     the profile. This is the exploration floor that pops the filter bubble.
//
// WHY a guaranteed floor rather than "hope rerank adds variety": for a concentrated
// query, recall returns ~300 same-category items, so EVERY candidate the ranking
// sees is that category. Variety cannot emerge from within the ranked pool — it has
// to be injected. Reserving a fixed slice guarantees it.
//
// Explore items are drawn at random from the store, excluding the dominant category,
// already-seen ids, and already-picked ids. They deliberately bypass
// Recall/Feature/Score (not ranked — that's the point of exploration), so their
// score columns stay zero. The RNG is seeded from the seen-set size, so the choice
// is reproducible for a given profile state and rotates as the user clicks more.
// -----------------------------------------------------------------------------
class MixOp : public Operator {
public:
    MixOp(const ItemStore& store, std::vector<std::uint32_t> seen_ids,
          std::size_t page_size, int new_ratio, std::size_t explore_floor)
        : store_(store),
          seen_(seen_ids.begin(), seen_ids.end()),
          page_size_(page_size),
          new_ratio_(new_ratio),
          explore_floor_(std::min(explore_floor, page_size)) {}

    std::string name() const override { return "MixOp"; }

    // Config-determined split (the store always has enough items to fill both pools),
    // so this stays const and needs no mutable per-run state.
    std::string detail() const override {
        return std::to_string(page_size_ - explore_floor_) + " exploit · " +
               std::to_string(explore_floor_) + " explore";
    }

protected:
    Batch transform(const Batch& in) const override {
        const std::size_t exploit_slots = page_size_ - explore_floor_;

        // --- Exploit: the new/seen mix, filling exploit_slots (not the whole page).
        std::vector<Candidate> new_items;
        std::vector<Candidate> seen_items;
        for (const Candidate& c : in.items) {
            (seen_.count(c.id) != 0 ? seen_items : new_items).push_back(c);
        }

        std::size_t new_target = static_cast<std::size_t>(
            std::llround(static_cast<double>(exploit_slots) * new_ratio_ / 100.0));
        if (new_target > exploit_slots) new_target = exploit_slots;

        std::size_t take_new = std::min(new_target, new_items.size());
        std::size_t take_seen = std::min(exploit_slots - new_target, seen_items.size());
        std::size_t shortfall = exploit_slots - take_new - take_seen;
        if (shortfall > 0) {  // backfill from the other pool, preferring more new
            const std::size_t extra_new = std::min(shortfall, new_items.size() - take_new);
            take_new += extra_new;
            shortfall -= extra_new;
        }
        if (shortfall > 0) {
            take_seen += std::min(shortfall, seen_items.size() - take_seen);
        }

        Batch out;
        out.items.reserve(page_size_);
        for (std::size_t i = 0; i < take_new; ++i) out.items.push_back(new_items[i]);
        for (std::size_t i = 0; i < take_seen; ++i) out.items.push_back(seen_items[i]);

        // --- Explore: inject explore_floor_ items from OUTSIDE the dominant category.
        if (explore_floor_ > 0 && store_.count() > 0) {
            // The dominant category = the one the exploited page is made of.
            std::vector<std::size_t> category_count(256, 0);
            for (const Candidate& c : out.items) category_count[store_.notes[c.id].category]++;
            std::uint8_t dominant = 0;
            std::size_t best = 0;
            for (std::size_t cat = 0; cat < category_count.size(); ++cat) {
                if (category_count[cat] > best) {
                    best = category_count[cat];
                    dominant = static_cast<std::uint8_t>(cat);
                }
            }

            std::unordered_set<std::uint32_t> picked;
            for (const Candidate& c : out.items) picked.insert(c.id);

            const std::size_t n = store_.count();
            std::mt19937 rng(static_cast<std::uint32_t>(seen_.size() + 1));
            std::size_t added = 0;
            for (std::size_t tries = 0; added < explore_floor_ && tries < n * 2; ++tries) {
                const std::uint32_t id = static_cast<std::uint32_t>(rng() % n);
                if (picked.count(id) != 0 || seen_.count(id) != 0) continue;
                if (store_.notes[id].category == dominant) continue;
                Candidate c;
                c.id = id;  // exploration item: intentionally unranked (score columns stay 0)
                out.items.push_back(c);
                picked.insert(id);
                ++added;
            }
        }

        return out;
    }

private:
    const ItemStore&                  store_;
    std::unordered_set<std::uint32_t> seen_;          // ids the user has already clicked
    std::size_t                       page_size_;     // final feed size
    int                               new_ratio_;     // target % of EXPLOIT slots that are new
    std::size_t                       explore_floor_; // guaranteed exploration slots
};
