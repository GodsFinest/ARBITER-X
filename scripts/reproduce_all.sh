#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
BUILD_DIR=${ARBITER_BUILD_DIR:-build-repro}
JOBS=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
mkdir -p artifacts
cmake --fresh -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DARBITER_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" --parallel "$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
"$BUILD_DIR/arbiter" verify --seed 42 --ms 50
"$BUILD_DIR/arbiter" fuzz --target differential --iters 3000
"$BUILD_DIR/arbiter" fuzz --target protocol --iters 200000
"$BUILD_DIR/arbiter" fuzz --target corpus
"$BUILD_DIR/arbiter" simulate --seed 42 --ms 100 --out artifacts/repro_sim
"$BUILD_DIR/arbiter" journal verify artifacts/repro_sim.journal
"$BUILD_DIR/arbiter" replay --journal artifacts/repro_sim.journal --book ref
"$BUILD_DIR/arbiter" replay --journal artifacts/repro_sim.journal --book opt
"$BUILD_DIR/arbiter" replay --journal artifacts/repro_sim.journal --until 20000
"$BUILD_DIR/arbiter" worldcheck --seed 4242 --events 20000 --out artifacts/repro.wsnap
"$BUILD_DIR/arbiter" worldjump --snap artifacts/repro.wsnap --events 20000
"$BUILD_DIR/arbiter" worldfork --snap artifacts/repro.wsnap --variable half_spread=4 --events 20000
for study in clustered-reactive poisson-reactive clustered-frozen; do
  "$BUILD_DIR/arbiter" flagship --reproduce "experiments/FLAGSHIP-$study/results.json"
done
"$BUILD_DIR/arbiter_bench" all > artifacts/benchmarks.log
cat artifacts/benchmarks.log
