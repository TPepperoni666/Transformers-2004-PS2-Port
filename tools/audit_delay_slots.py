"""Delay-slot split audit for the function map (read-only).

Origin: upstream runtime integration notes (KIexster/god-of-war-recomp note; GPL-3.0 project).
A function map row that starts at a branch's delay slot makes upstream PS2Recomp drop that slot from the
enclosing function (2026-10-08: material pointer a1 never set at 0x200048 -> black hub characters).
Since the 2026-10-08 regen bundle the worktree recompiler decodes such an out-of-range slot anyway and logs
a `decode` warning, but the map should still be kept clean.

Usage:
    python tools/audit_delay_slots.py [map.csv] [elf]
Defaults: transformers_map.csv, SLUS_206.68. Exit code 1 if a real split is found
(the stray `jr $ra` padding case is reported but not counted).
"""
import csv
import struct
import sys

map_path = sys.argv[1] if len(sys.argv) > 1 else 'transformers_map.csv'
elf_path = sys.argv[2] if len(sys.argv) > 2 else 'SLUS_206.68'

f = open(elf_path, 'rb').read()
ph = struct.unpack_from('<I', f, 0x1c)[0]
es, n = struct.unpack_from('<HH', f, 0x2a)
segs = [(v, o, fs) for t, o, v, p, fs, ms, fl, al in
        (struct.unpack_from('<8I', f, ph + i * es) for i in range(n)) if t == 1 and v]


def word(a):
    for v, o, fs in segs:
        if v <= a < v + fs:
            return struct.unpack_from('<I', f, o + a - v)[0]
    return None


def has_delay_slot(w):
    op = w >> 26
    return (op in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or
            (op == 0 and (w & 0x3f) in (8, 9)) or
            (op in (0x10, 0x11, 0x12) and ((w >> 21) & 31) == 8))  # COP0/1/2 BC (bc0/bc1/bc2 f/t/l)


def is_jr_ra(w):
    return w == 0x03E00008


real = 0
rows = 0
for r in csv.reader(open(map_path)):
    if not r or r[0] == 'Name':
        continue
    rows += 1
    s = int(r[1], 16)
    pw = word(s - 4)
    if pw is None or not has_delay_slot(pw):
        continue
    # A `jr $ra` right before a function start is normally the previous function's return whose own
    # delay slot sits at s-0... only when s-8 is not itself a branch is the word at s-4 "stray" padding.
    ppw = word(s - 8)
    kind = 'REAL SPLIT'
    if is_jr_ra(pw):
        kind = 'jr $ra before start (check: stray padding vs real split)'
    if is_jr_ra(pw) and ppw is not None and not has_delay_slot(ppw):
        # `... ; jr $ra ; <s>` : if <s> is a NOP-free real instruction this could still be a split; report.
        kind = 'jr $ra before start (stray padding if the previous function already returned)'
    else:
        real += 1 if kind == 'REAL SPLIT' else 0
    print(f'{r[0]:<24} start={s:#010x} prev_word={pw:#010x} {kind}')

print(f'rows={rows} real_splits={real}')
sys.exit(1 if real else 0)
