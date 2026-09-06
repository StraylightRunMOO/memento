#!/usr/bin/env bash
# Clone daanx/mimalloc-bench, build its in-tree tests plus LD_PRELOAD
# shims for memento / mimalloc / rpmalloc, and run a practical subset.
#
# Full `allt` (lean, redis, rocksdb, ghostscript, every allocator) needs
# extra packages and RAM; this script does the local C tests plus the
# three allocators we already vendor. Re-run with EXTRA_TESTS=... to
# add more, or drop into mimalloc-bench/out/bench and invoke bench.sh
# yourself (see BENCHMARKS.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXTERN="${EXTERN:-$ROOT/extern}"
MB="$EXTERN/mimalloc-bench"
SHIMS="$ROOT/bench/shims"
RESULTS="$ROOT/bench/results"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
# Tests that compile from mimalloc-bench/bench without extra downloads.
TESTS="${TESTS:-cfrac espresso barnes alloc-test glibc-simple glibc-thread mstress rptest cscratch malloc-large larson}"

mkdir -p "$EXTERN" "$SHIMS" "$RESULTS"

if [[ ! -d "$MB/.git" ]]; then
    echo "==> cloning mimalloc-bench"
    git clone --depth 1 https://github.com/daanx/mimalloc-bench.git "$MB"
fi

echo "==> building LD_PRELOAD shims"
# memento (sized malloc, the fair LD_PRELOAD comparison)
gcc -O3 -march=native -DNDEBUG -fPIC -shared -std=c11 \
    -I "$ROOT/include" \
    "$ROOT/shim/memento_malloc.c" \
    -o "$SHIMS/memento_malloc.so" -lpthread

gcc -O3 -march=native -DNDEBUG -DMEMENTO_STATS=0 -fPIC -shared -std=c11 \
    -I "$ROOT/include" \
    "$ROOT/shim/memento_malloc.c" \
    -o "$SHIMS/memento_malloc_nostats.so" -lpthread

# mimalloc 2.x shared (reuse FetchContent source if present)
MI_SRC=""
for cand in \
    "$ROOT/build-bench/_deps/mimalloc-src" \
    "$ROOT/build/_deps/mimalloc-src"; do
    if [[ -d "$cand" ]]; then MI_SRC="$cand"; break; fi
done
if [[ -z "$MI_SRC" ]]; then
    echo "error: mimalloc source not found; build the compare suite first" >&2
    exit 1
fi
cmake -S "$MI_SRC" -B "$SHIMS/mimalloc-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMI_BUILD_SHARED=ON -DMI_BUILD_STATIC=OFF -DMI_BUILD_OBJECT=OFF \
    -DMI_BUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS="-O3 -march=native" \
    >/dev/null
cmake --build "$SHIMS/mimalloc-build" -j "$JOBS"
MI_SO="$(find "$SHIMS/mimalloc-build" -name 'libmimalloc.so*' -type f | head -1)"
if [[ -z "$MI_SO" ]]; then
    echo "error: libmimalloc.so not built" >&2
    exit 1
fi
ln -sfn "$MI_SO" "$SHIMS/libmimalloc.so"

# rpmalloc wrap: ENABLE_OVERRIDE includes malloc.c
RP_SRC=""
for cand in \
    "$ROOT/build-bench/_deps/rpmalloc-src" \
    "$ROOT/build/_deps/rpmalloc-src"; do
    if [[ -d "$cand" ]]; then RP_SRC="$cand"; break; fi
done
if [[ -z "$RP_SRC" ]]; then
    echo "error: rpmalloc source not found; build the compare suite first" >&2
    exit 1
fi
gcc -O3 -march=native -DNDEBUG -fPIC -shared -std=gnu11 \
    -D_GNU_SOURCE -DENABLE_OVERRIDE=1 -DENABLE_PRELOAD=1 \
    -I "$RP_SRC/rpmalloc" \
    "$RP_SRC/rpmalloc/rpmalloc.c" \
    -o "$SHIMS/librpmallocwrap.so" -lpthread -ldl

cat > "$RESULTS/external.allocs" <<EOF
me $SHIMS/memento_malloc.so
me0 $SHIMS/memento_malloc_nostats.so
mi $SHIMS/libmimalloc.so
rp $SHIMS/librpmallocwrap.so
EOF
echo "==> external allocators:"
cat "$RESULTS/external.allocs"

echo "==> building mimalloc-bench local tests"
mkdir -p "$MB/extern"
: >> "$MB/extern/versions.txt"
# sh6/sh8 sources are downloaded by build-bench-env.sh (needs dos2unix).
# Drop those targets so cmake of the rest of bench/ still works. Skip the
# security/ tree too — we don't run it and it compiles ~90 extra binaries.
python3 - "$MB/bench/CMakeLists.txt" <<'PY'
import pathlib, re, sys
p = pathlib.Path(sys.argv[1])
text = p.read_text()
if "sh6bench-new.c" in text:
    src = p.parent / "shbench" / "sh6bench-new.c"
    if not src.is_file():
        text = re.sub(r"\n  add_executable\(sh6bench.*?pthread\)\n", "\n", text, flags=re.S)
        text = re.sub(r"\n  add_executable\(sh8bench.*?pthread\)\n", "\n", text, flags=re.S)
text = text.replace("add_subdirectory(security)\n", "")
p.write_text(text)
PY
CC="${CC:-gcc}"
CXX="${CXX:-g++}"
cmake -B "$MB/out/bench" -S "$MB/bench" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_C_FLAGS="-O3 -march=native" \
    -DCMAKE_CXX_FLAGS="-O3 -march=native" \
    >/dev/null
cmake --build "$MB/out/bench" -j "$JOBS"

# GNU time stand-in (Ubuntu docker images often ship only the shell builtin)
export PATH="$ROOT/bench/bin:$PATH"
chmod +x "$ROOT/bench/bin/time"

echo "==> running: sys + --external (me me0 mi rp) $TESTS"
cd "$MB/out/bench"
# --external already adds me/me0/mi/rp to alloc_run. Passing those names
# as flags either warns (not in alloc_all) or double-runs (mi/rp are).
# shellcheck disable=SC2086
../../bench.sh sys --external="$RESULTS/external.allocs" $TESTS

if [[ -f "$MB/out/bench/benchres.csv" ]]; then
    cp "$MB/out/bench/benchres.csv" "$RESULTS/mimalloc-bench-allt.csv"
    echo "==> copied results to $RESULTS/mimalloc-bench-allt.csv"
    python3 "$ROOT/bench/plot.py"
fi
