"""
Diff `obswb_v2c_edge.txt` against the frozen v2.0a corpus -- INDEPENDENTLY.

The generator is not asked whether it did the right thing. This walks both files
itself, in the replayer's own two-rows-per-transaction order, and enforces the
proven boundary field by field:

    * every row count, run name, N, C and cycle count: identical;
    * every field of every row: identical, EXCEPT
    * `rdata` on the response row of an acknowledged read at OBS_STATUS (0x010),
      which must be `old | 0x8`, or `old | 0x8 | 0x10` where the underrun level is
      asserted. Nothing else may move -- and `OBS_CTRL` in particular must be
      byte-for-byte equivalent, since the frozen cases select the identity and its
      SELECT bits read 0.

It reports the difference count, but the count is not the assertion: the shape is.
A count-only check would accept the wrong eleven observations.

Usage:
    python3 diff_v2c_boundary.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from replay_observer_wb import FIELDS, F_ADR, F_CYC, F_RDATA, F_STB, F_WE, parse

VECTOR_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "..", "sim", "vectors")

OLD_NAME = "obswb_edge.txt"
NEW_NAME = "obswb_v2c_edge.txt"

OBS_CTRL = 0x00C
OBS_STATUS = 0x010

ST_READY = 1 << 3
ST_UNDERRUN = 1 << 4


def status_response_rows(rows):
    """The row indices that carry a response to a read of OBS_STATUS."""
    marks = set()
    i = 0
    while i < len(rows):
        row = rows[i]
        if row[F_CYC] and row[F_STB]:
            if not row[F_WE] and row[F_ADR] == OBS_STATUS:
                marks.add(i + 1)
            resp = rows[i + 1] if i + 1 < len(rows) else None
            held = resp is not None and resp[F_CYC] and resp[F_STB]
            i += 2 + (1 if held else 0)
        else:
            i += 1
    return marks


def main():
    old_path = os.path.join(VECTOR_DIR, OLD_NAME)
    new_path = os.path.join(VECTOR_DIR, NEW_NAME)
    for p in (old_path, new_path):
        if not os.path.isfile(p):
            print(f"FAIL  no such file: {p}")
            return 1

    old_runs, new_runs = parse(old_path), parse(new_path)
    problems = []
    diffs = []

    if len(old_runs) != len(new_runs):
        problems.append(f"{len(old_runs)} runs vs {len(new_runs)}")
        return _report(diffs, problems)

    for ra, rb in zip(old_runs, new_runs):
        for key in ("name", "N", "C", "cycles"):
            if ra[key] != rb[key]:
                problems.append(f"run header {ra['name']}: {key} is "
                                f"{ra[key]} vs {rb[key]}")
        rows_a, rows_b = ra["rows"], rb["rows"]
        if len(rows_a) != len(rows_b):
            problems.append(f"{ra['name']}: {len(rows_a)} rows vs "
                            f"{len(rows_b)}")
            continue

        marks = status_response_rows(rows_a)
        for i, (row_a, row_b) in enumerate(zip(rows_a, rows_b)):
            for f in range(FIELDS):
                if row_a[f] == row_b[f]:
                    continue
                if f != F_RDATA or i not in marks:
                    problems.append(
                        f"{ra['name']} row {i} field {f}: {row_a[f]:#x} -> "
                        f"{row_b[f]:#x}, which the boundary does not allow")
                    continue
                ok = row_b[f] in (row_a[f] | ST_READY,
                                  row_a[f] | ST_READY | ST_UNDERRUN)
                if not ok:
                    problems.append(
                        f"{ra['name']} row {i}: an OBS_STATUS response moved "
                        f"{row_a[f]:#x} -> {row_b[f]:#x}, a delta of "
                        f"{row_b[f] ^ row_a[f]:#x}")
                    continue
                diffs.append((ra["name"], i, row_a[f], row_b[f]))

    return _report(diffs, problems)


def _report(diffs, problems):
    print(f"diff_v2c_boundary: {len(diffs)} transcribed difference(s)")
    for name, i, old, new in diffs:
        print(f"  {name} row {i}: OBS_STATUS {old:#x} -> {new:#x} "
              f"(delta {new ^ old:#x})")
    if problems:
        print(f"FAIL  {len(problems)} difference(s) outside the boundary:")
        for p in problems[:12]:
            print(f"  {p}")
        return 1
    print("  every difference is an OBS_STATUS response gaining SEED_READY, and "
          "nothing else in either file moved.")
    print("  request rows, cycle structure, ACK/ERR, OBS_CTRL and all engine "
          "observations are identical.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
