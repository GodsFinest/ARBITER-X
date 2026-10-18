#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=${ARBITER_BUILD_DIR:-build-repro}
JOBS=${CMAKE_BUILD_PARALLEL_LEVEL:-8}

mkdir -p artifacts

printf '%s\n' '== clean release build =='
cmake --fresh -S . -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DARBITER_WARNINGS_AS_ERRORS=ON
cmake --build "$BUILD_DIR" -j "$JOBS"

printf '%s\n' '== complete test suite =='
ctest --test-dir "$BUILD_DIR" --output-on-failure

printf '%s\n' '== deterministic world and adversarial checks =='
"$BUILD_DIR/arbiter" verify --seed 42 --ms 50
"$BUILD_DIR/arbiter" fuzz --target differential --iters 3000
"$BUILD_DIR/arbiter" fuzz --target protocol --iters 200000
"$BUILD_DIR/arbiter" fuzz --target corpus

printf '%s\n' '== journal, replay, and time travel =='
"$BUILD_DIR/arbiter" simulate --seed 42 --ms 100 --out artifacts/repro_sim
"$BUILD_DIR/arbiter" journal verify artifacts/repro_sim.journal
"$BUILD_DIR/arbiter" replay --journal artifacts/repro_sim.journal --book ref
"$BUILD_DIR/arbiter" replay --journal artifacts/repro_sim.journal --book opt
"$BUILD_DIR/arbiter" timetravel --journal artifacts/repro_sim.journal --event 20000

printf '%s\n' '== research artifacts =='
"$BUILD_DIR/arbiter" research validate --csv data/golden/imbalance_case_001.csv
"$BUILD_DIR/arbiter" research reproduce EXP-001
"$BUILD_DIR/arbiter" research reproduce EXP-002
"$BUILD_DIR/arbiter" research reproduce EXP-003
if command -v python3 >/dev/null 2>&1; then
  python3 tools/research/reference_features.py
  python3 tools/research/analyze.py experiments/EXP-001
fi

printf '%s\n' '== flagship artifacts =='
"$BUILD_DIR/arbiter" flagship --reproduce experiments/FLAGSHIP-clustered-reactive/results.json
"$BUILD_DIR/arbiter" flagship --reproduce experiments/FLAGSHIP-poisson-reactive/results.json
"$BUILD_DIR/arbiter" flagship --reproduce experiments/FLAGSHIP-clustered-frozen/results.json

printf '%s\n' '== benchmarks =='
"$BUILD_DIR/arbiter_bench" all | tee artifacts/benchmarks.log

printf '%s\n' '== reproduction complete =='