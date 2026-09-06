#!/usr/bin/env bash
# Build Release compare_{glibc,mimalloc,rpmalloc,memento,memento_nostats},
# run each RUNS times (default 3), keep the min, emit CSV + graphs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD:-$ROOT/build-bench}"
RESULTS="$ROOT/bench/results"
RAW="$RESULTS/raw"
RUNS="${RUNS:-3}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

mkdir -p "$RAW" "$RESULTS" "$ROOT/bench/images"

fc_flags=()
for name_dir in \
    MIMALLOC:"$ROOT/build/_deps/mimalloc-src" \
    RPMALLOC:"$ROOT/build/_deps/rpmalloc-src" \
    NANOBENCH:"$ROOT/build/_deps/nanobench-src"; do
    name="${name_dir%%:*}"
    dir="${name_dir#*:}"
    if [[ -d "$dir" ]]; then
        fc_flags+=("-DFETCHCONTENT_SOURCE_DIR_${name}=$dir")
    fi
done

# Prefer gcc: the rest of the tree is documented against it, and `cc` on
# this box is Clang 22 which is a different code-gen than the README table.
CC="${CC:-gcc}"
CXX="${CXX:-g++}"

if [[ ! -x "$BUILD/bench/compare_memento" ]]; then
    echo "==> configuring Release bench tree at $BUILD (CC=$CC)"
    cmake -S "$ROOT" -B "$BUILD" \
        -DBUILD_BENCHMARKS=ON -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER="$CC" \
        -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_C_FLAGS="-O3 -march=native" \
        -DCMAKE_CXX_FLAGS="-O3 -march=native" \
        "${fc_flags[@]}"
fi

echo "==> building compare binaries"
cmake --build "$BUILD" --target \
    compare_glibc compare_mimalloc compare_rpmalloc \
    compare_memento compare_memento_nostats \
    -j "$JOBS"

BIN="$BUILD/bench"
BINS=(compare_glibc compare_mimalloc compare_rpmalloc compare_memento compare_memento_nostats)

echo "==> $RUNS runs per allocator (min is kept)"
for ((n = 1; n <= RUNS; n++)); do
    for b in "${BINS[@]}"; do
        echo "  run $n / $b"
        "$BIN/$b" | tee "$RAW/run${n}-${b}.txt"
    done
done

echo "==> collecting min-of-$RUNS and plotting"
python3 "$ROOT/bench/plot.py" --collect-compare "$RAW"
echo "CSV:    $RESULTS/compare.csv"
echo "graphs: $ROOT/bench/images"
