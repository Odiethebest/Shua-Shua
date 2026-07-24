// -----------------------------------------------------------------------------
// bindings.cpp — the WebAssembly boundary (Milestone M3, Part B).
//
// This is embind GLUE, not engine logic: it exposes the engine's existing entry
// points (from api.hpp) to JavaScript. Compiled ONLY by Emscripten (see
// scripts/build-wasm.sh); the native build ignores it. The native front door is
// main.cpp; this is the browser/Node front door. Both call the same api.hpp.
//
// Note: on the wasm target __ARM_NEON is not defined, so dot_simd falls back to
// the scalar kernel (see dot.hpp) — WASM SIMD is a possible later enhancement.
// -----------------------------------------------------------------------------

#include <emscripten/bind.h>

#include <cstddef>
#include <string>

#include "api.hpp"

// Run the cascade for a persona and return the JSON payload. JS wants the string
// (it will JSON.parse it), so recommend + to_json are combined into one call.
static std::string recommend_json(int persona_id) {
    return to_json(recommend(persona_id));
}

// Item-based recall: recommend items similar to a clicked item (query = that
// item's own vector). Returns the same JSON shape as recommend().
static std::string recommend_similar_json(int item_id) {
    return to_json(recommend_similar(item_id));
}

// How many personas the switcher can offer, and their labels — so the UI can
// build the persona control without hard-coding the list.
static int persona_count() {
    return static_cast<int>(personas().size());
}

static std::string persona_label(int persona_id) {
    const std::vector<Persona>& list = personas();
    if (persona_id < 0 || static_cast<std::size_t>(persona_id) >= list.size()) {
        return "";
    }
    return list[static_cast<std::size_t>(persona_id)].label;
}

EMSCRIPTEN_BINDINGS(shuashua) {
    emscripten::function("recommend", &recommend_json);
    emscripten::function("recommendSimilar", &recommend_similar_json);
    emscripten::function("personaCount", &persona_count);
    emscripten::function("personaLabel", &persona_label);
}
