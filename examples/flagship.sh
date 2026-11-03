#!/bin/sh
set -eu

cd "$(dirname "$0")/.."

BUILD_DIR=${1:-./build}
ARB="$BUILD_DIR/arbiter"
OUT=artifacts/flagship

mkdir -p "$OUT"

printf '%s\n' '== baseline and full-world checkpoint =='
"$ARB" simulate --seed 4242 --ms 100 --out "$OUT/baseline"
"$ARB" journal verify "$OUT/baseline.journal"
"$ARB" worldcheck --seed 4242 --events 20000 --out "$OUT/ck20000.wsnap"

printf '%s\n' '== deterministic restore =='
"$ARB" worldjump --snap "$OUT/ck20000.wsnap" --events 20000
"$ARB" worldjump --snap "$OUT/ck20000.wsnap" --events 20000

printf '%s\n' '== counterfactual fork =='
"$ARB" worldfork --snap "$OUT/ck20000.wsnap" \
  --variable half_spread=4 \
  --events 40000

printf '%s\n' '== fixed-total allocation and exact reproduction =='
"$ARB" flagship --config configs/flagship_clustered.axexp --out "$OUT/allocation"
"$ARB" flagship --reproduce "$OUT/allocation/results.json"
printf 'output: %s\n' "$OUT"
