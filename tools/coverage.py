#!/usr/bin/env python3
"""Union line-coverage report for memento, from an existing gcov-instrumented build.

Usage:
    cmake -B build-cov -DCMAKE_BUILD_TYPE=Debug -DMEMENTO_COVERAGE=ON
    cmake --build build-cov -j
    cd build-cov && ctest --output-on-failure && cd ..
    python3 tools/coverage.py build-cov

Prints per-file union coverage across every test binary. The allocator itself
(include/memento.h) is the number that matters; the union across all suites is
the honest metric (each suite alone exercises a slice of the surface).
"""

import glob
import os
import re
import subprocess
import sys

LINE_RE = re.compile(r'^\s*([0-9]+|#####|=====|\$+|-):\s*(\d+):(.*)$')


def collect(build_dir):
    executed = {}  # (source_path, line_no) -> total hits
    for gcno in glob.glob(f'{build_dir}/**/CMakeFiles/*/*.gcno', recursive=True):
        gdir = gcno.rsplit('/', 1)[0]
        res = subprocess.run(['gcov', '-t', gcno], cwd=gdir,
                             capture_output=True, text=True)
        cur = None
        for line in res.stdout.splitlines():
            if ':Source:' in line:
                cur = line.split('Source:')[1].strip()
                continue
            m = LINE_RE.match(line)
            if not m or cur is None:
                continue
            cnt, ln = m.group(1), int(m.group(2))
            if cnt == '-':
                continue
            hit = 0 if cnt in ('#####', '=====') else max(
                1, int(re.sub(r'\D', '', cnt) or 1))
            key = (cur, ln)
            executed[key] = executed.get(key, 0) + hit
    return executed


def main():
    build_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else 'build-cov')
    executed = collect(build_dir)
    if not executed:
        sys.exit(f'no coverage data under {build_dir}; see header comment')

    by_file = {}
    for (src, ln), c in executed.items():
        tot, cov = by_file.get(src, (0, 0))
        by_file[src] = (tot + 1, cov + (c > 0))

    rows = sorted(by_file.items(), key=lambda kv: kv[0])
    width = max(len(s) for s in by_file)
    gt = gc = 0
    for src, (tot, cov) in rows:
        gt += tot
        gc += cov
        print(f'{src:<{width}}  {cov:>5}/{tot:<5}  {100.0 * cov / tot:5.1f}%')
    print('-' * (width + 18))
    print(f'{"TOTAL":<{width}}  {gc:>5}/{gt:<5}  {100.0 * gc / gt:5.1f}%')


if __name__ == '__main__':
    main()
