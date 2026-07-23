#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Candidate: one item flowing through the cascade, carrying the columns each
// stage fills in. Fields are populated progressively — similarity by RecallOp,
// the feature columns by FeatureOp, score by ScoreOp — and default to 0 before
// their producing stage has run.
//
// WHY an array-of-structs batch, when the item *store* is SoA: the SoA rule
// exists so the similarity kernel can stream item vectors contiguously. This
// batch is a small, transient candidate set that stages read and write
// field-by-field and that never touches the hot kernel, so a plain struct per
// candidate is the more readable layout and the SoA concern does not apply here.
// -----------------------------------------------------------------------------
struct Candidate {
    std::uint32_t id;                    // item index into the ItemStore
    float         similarity = 0.0f;     // RecallOp: dot-product similarity to the query
    float         category_match = 0.0f; // FeatureOp: user affinity for this category
    float         recency = 0.0f;        // FeatureOp: freshness in (0, 1]
    float         popularity = 0.0f;     // FeatureOp: normalized popularity in [0, 1]
    float         score = 0.0f;          // ScoreOp: combined ranking score
};

// Batch: the payload passed between operators — batch in, batch out.
struct Batch {
    std::vector<Candidate> items;
};

// -----------------------------------------------------------------------------
// TraceEntry: one operator's self-report. TRACING IS A FIRST-CLASS OUTPUT.
// The field set is a fixed contract the UI depends on — do not change it here
// without updating the frontend.
// -----------------------------------------------------------------------------
struct TraceEntry {
    std::string                name;        // operator name, e.g. "RecallOp"
    std::size_t                in_count;    // candidates received
    std::size_t                out_count;   // candidates emitted
    double                     latency_us;  // wall-clock time spent, microseconds
    std::vector<std::uint32_t> sample_ids;  // a few output ids, for the UI to show
};

// -----------------------------------------------------------------------------
// Operator: the uniform pipeline-stage interface.
//
// The public entry point run() is defined ONCE here (the template-method
// pattern): it times the stage, calls the stage-specific transform(), and
// appends exactly one TraceEntry in the fixed shape. Subclasses implement only
// transform() and name() — the actual algorithm — and cannot forget or diverge
// on how the trace is recorded.
//
// WHY centralize tracing in the base rather than let each operator time itself
// and push its own TraceEntry: tracing is the product, and the record
// {name, in_count, out_count, latency_us, sample_ids} must be identical for
// every stage. Producing it in one place guarantees that. The rejected
// alternative — per-operator trace assembly — duplicates the boilerplate four
// times and lets a stage silently report inconsistently or omit sample_ids.
// -----------------------------------------------------------------------------
class Operator {
public:
    virtual ~Operator() = default;

    // Stable operator name, shown in the trace and the UI.
    virtual std::string name() const = 0;

    // Run the stage over `in`, appending this stage's TraceEntry to `trace`.
    Batch run(const Batch& in, std::vector<TraceEntry>& trace) const {
        // steady_clock: monotonic, so a wall-clock adjustment mid-run cannot make
        // a measured duration jump or go negative. system_clock would usually
        // read the same but is the wrong tool for a stopwatch.
        const auto start = std::chrono::steady_clock::now();
        Batch out = transform(in);
        const auto end = std::chrono::steady_clock::now();

        TraceEntry entry;
        entry.name = name();
        entry.in_count = in.items.size();
        entry.out_count = out.items.size();
        entry.latency_us = std::chrono::duration<double, std::micro>(end - start).count();
        entry.sample_ids = first_ids(out);
        trace.push_back(std::move(entry));
        return out;
    }

protected:
    // The stage-specific transform. Subclasses implement exactly this.
    virtual Batch transform(const Batch& in) const = 0;

private:
    // The first few output ids, so the UI can show which items a stage let
    // through without carrying the whole batch in the trace.
    static std::vector<std::uint32_t> first_ids(const Batch& out) {
        const std::size_t kSampleCount = 5;
        const std::size_t n = std::min(kSampleCount, out.items.size());
        std::vector<std::uint32_t> ids;
        ids.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            ids.push_back(out.items[i].id);
        }
        return ids;
    }
};
