cmake_minimum_required(VERSION 3.20)
project(shuashua CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# M0 kernel: one executable built from src/. The headers are header-only for now
# and are listed so the IDE associates them with this target (that is what clears
# CLion's "does not belong to any project target" banner). Keep this to a single
# target — later milestones (M1 DAG, M3 WASM) grow the build; don't pull that in
# early. The documented command-line build remains the plain clang++ one-liner.
add_executable(shuashua
    src/main.cpp
    src/note.hpp
    src/item_store.hpp
    src/operator.hpp
    src/recall_op.hpp
    src/feature_op.hpp
    src/score_op.hpp
    src/rerank_op.hpp
    src/scheduler.hpp
)

target_include_directories(shuashua PRIVATE src)
