#!/usr/bin/env python3
"""Regenerate the size-class lookup table in include/memento.h.

The LUT maps 16-byte buckets to size-class indices: for an allocation
request of `size` bytes (1..MEMENTO_MAX_SIZE_CLASS), the class index is
memento_sc_lut[(size + 15) >> 4]. The table is derived from the
memento_size_classes[] array in the header and written back in place.

Hand-editing the LUT once produced a silent heap overflow (a 4096-byte
request landing in a 768-byte class). Do not hand-edit; run this:

    python3 tools/gen_lut.py [--check]

--check verifies the in-header LUT matches the class table without
rewriting anything (exit 1 on mismatch).
"""

import re
import sys

HEADER = "include/memento.h"
LUT_NAME = "memento_sc_lut"


def load(path):
    with open(path) as f:
        return f.read()


def parse_classes(src):
    m = re.search(
        r"static const size_t memento_size_classes\[\w+\] = \{([^}]*)\};", src
    )
    if not m:
        sys.exit("error: memento_size_classes table not found")
    classes = [int(x) for x in re.findall(r"\d+", m.group(1))]
    if classes != sorted(classes) or len(set(classes)) != len(classes):
        sys.exit("error: class table must be strictly increasing")
    if any(c % 16 != 0 for c in classes):
        sys.exit("error: every class must be a multiple of 16 (LUT buckets)")
    if len(classes) > 255:
        sys.exit("error: class count must fit in uint8_t")
    return classes


def build_lut(classes):
    max_size = classes[-1]
    buckets = max_size // 16 + 1
    lut = []
    for idx in range(buckets):
        target = max(1, idx * 16)
        cls = next(i for i, c in enumerate(classes) if c >= target)
        lut.append(cls)
    return lut


def verify(classes, lut):
    for size in range(1, classes[-1] + 1):
        cls = lut[(size + 15) >> 4]
        assert classes[cls] >= size, (size, classes[cls])
        if cls > 0:
            assert classes[cls - 1] < size, (size, classes[cls - 1])
    return True


def format_lut(lut):
    lines = []
    for i in range(0, len(lut) - 1, 16):
        lines.append(
            "    " + ", ".join(f"{v:2d}" for v in lut[i : i + 16]) + ","
        )
    lines.append(f"    {lut[-1]:2d}")
    return "\n".join(lines)


def main():
    check_only = "--check" in sys.argv
    src = load(HEADER)
    classes = parse_classes(src)
    lut = build_lut(classes)
    verify(classes, lut)

    pat = re.compile(
        r"(static const uint8_t " + LUT_NAME + r"\[\d+\] = \{\n).*?(\n\};)",
        re.S,
    )
    m = pat.search(src)
    if not m:
        sys.exit(f"error: {LUT_NAME} not found in {HEADER}")

    new = m.group(1) + format_lut(lut) + m.group(2)
    if check_only:
        if m.group(0) == new:
            print(f"LUT is in sync ({len(classes)} classes)")
            return
        sys.exit("error: LUT out of sync — run tools/gen_lut.py")
    src = src[: m.start()] + new + src[m.end() :]
    with open(HEADER, "w") as f:
        f.write(src)
    print(f"regenerated LUT for {len(classes)} classes "
          f"(max {classes[-1]} bytes)")


if __name__ == "__main__":
    main()
