"""Compare the guest-time timelines of two scripted runs (PLAN.md section 4).

Usage: python tools/script_compare.py <first_log_path> <second_log_path>
Reads the two private log files, keeps the [script] lines, drops host-time fields (t=..s) and dump paths, and reports the
first line where the two runs differ. A deterministic harness gives IDENTICAL for the same build + script + fresh card,
whatever the host load, GS worker on/off, or which run started first.
"""
import re
import sys

HOST_FIELDS = re.compile(r"\s+t=[0-9.]+s|\s+wall=[0-9.]+")


def timeline(run):
    out = []
    with open(run, errors="ignore") as f:
        for line in f:
            if not line.startswith("[script]"):
                continue
            if line.startswith("[script] dump ") or line.startswith("[script] exit watchdog") \
                    or line.startswith("[script] state") or line.startswith("[script] thread"):
                continue  # host paths / racy diagnostics
            out.append(HOST_FIELDS.sub("", line.rstrip()))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    a, b = timeline(sys.argv[1]), timeline(sys.argv[2])
    if not a or not b or not any('RESULT' in line for line in a) or not any('RESULT' in line for line in b):
        print('Both logs must contain a completed [script] RESULT timeline')
        return 1
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            print(f"DIVERGE at [script] line {i}:\n  {sys.argv[1]}: {x}\n  {sys.argv[2]}: {y}")
            return 1
    if len(a) != len(b):
        print(f"PREFIX-IDENTICAL ({min(len(a), len(b))} lines); lengths {len(a)} vs {len(b)}")
        return 1
    print(f"IDENTICAL ({len(a)} [script] lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
