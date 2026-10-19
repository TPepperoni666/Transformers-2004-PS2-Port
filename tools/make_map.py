"""Build transformers_map.csv (Ghidra-style function map) + transformers_mapped.toml.

The analyzer lumped 0x670250..end into one bogus function, so the second code block
(C++ static constructors, reached only via the ctor table at 0x6ADF20) was never split.
When a map is loaded the recompiler drops every auto-detected function not in it, so the
map carries all the good functions from output_clean plus the newly found block-2 starts.
"""
import os, re, struct, argparse
from pathlib import Path

parser=argparse.ArgumentParser(description='Refine a locally generated Transformers function map')
parser.add_argument('--elf',required=True)
parser.add_argument('--baseline',required=True)
parser.add_argument('--config',required=True)
parser.add_argument('--out-map',required=True)
parser.add_argument('--out-config',required=True)
parser.add_argument('--output',required=True)
args=parser.parse_args()
from elftools.elf.elffile import ELFFile

m = ELFFile(open(args.elf, 'rb')).get_section_by_name('main').data()
w = lambda a: struct.unpack_from('<I', m, a - 0x100000)[0]

CTOR_TABLE = (0x6ADF20, 0x6AE608)   # a0/a1 at the static-init loop (0x10a190)
ctors = sorted({w(a) for a in range(*CTOR_TABLE, 4)})
BLOCK2 = (ctors[0], CTOR_TABLE[0])
RANGES = [(0x100000, 0x5DCB80), (0x618000, 0x619BC8), BLOCK2]

def range_of(a):
    return next((r for r in RANGES if r[0] <= a < r[1]), None)

# existing functions (name, start) from the last good recompile, minus anything in data
funcs = {}
for f in os.listdir(args.baseline):
    mm = re.match(r'(.+)_0x([0-9a-f]+)\.cpp$', f)
    if not mm:
        continue
    a = int(mm.group(2), 16)
    if range_of(a) and a < BLOCK2[0]:
        funcs[a] = mm.group(1)

# block 2 starts: ctor table, after each jr ra + delay slot, and lui/addiu pointers into it
new = set(ctors)
for a in range(BLOCK2[0], BLOCK2[1] - 8, 4):
    if w(a) == 0x03E00008:
        b = a + 8
        while b < BLOCK2[1] and w(b) == 0:   # skip alignment padding
            b += 4
        if b < BLOCK2[1]:
            new.add(b)
for lo, hi in RANGES:
    for a in range(lo, hi - 4, 4):
        x = w(a)
        if x >> 26 == 0xF:                      # lui rt, hi
            rt, hi16 = (x >> 16) & 31, x & 0xFFFF
            for b in range(a + 4, min(a + 32, hi), 4):
                y = w(b)
                if y >> 26 == 9 and (y >> 21) & 31 == rt:   # addiu rX, rt, lo
                    ptr = ((hi16 << 16) + ((y & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                    if BLOCK2[0] <= ptr < BLOCK2[1] and ptr % 4 == 0:
                        new.add(ptr)
                    break
for a in new:
    funcs.setdefault(a, ('ctor_%08X' if a in ctors else 'fn_%08X') % a)

# --- split out functions only reachable indirectly (vtables, callbacks) -----------------
# Candidate start = first non-zero word after a `jr ra` + delay slot, that is either
# pointed to from data / a lui+addiu pair, or opens a stack frame (addiu sp,sp,-N).
# Accept only if no branch in the enclosing function crosses the split point: true for a
# real function boundary, almost never for a switch-case label after a mid-function return.
def branch_target(a, x):
    op = x >> 26
    off = ((x & 0xFFFF) ^ 0x8000) - 0x8000
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or \
       (op == 1 and (x >> 16) & 31 in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)) or \
       (op in (0x10, 0x11, 0x12) and (x >> 21) & 31 == 8):
        return a + 4 + (off << 2)
    if op == 2:                                   # j (intra-function jump or tail call)
        return ((a + 4) & 0xF0000000) | ((x & 0x3FFFFFF) << 2)
    return None

code_ptrs = set()
for a in range(0x100000, 0x100000 + len(m) - 3, 4):
    if not range_of(a):                           # data words that look like code addresses
        x = w(a)
        if x % 4 == 0 and range_of(x):
            code_ptrs.add(x)
for lo, hi in RANGES:                             # lui/addiu pointer materialisation in code
    for a in range(lo, hi - 4, 4):
        x = w(a)
        if x >> 26 == 0xF:
            rt, hi16 = (x >> 16) & 31, x & 0xFFFF
            for b in range(a + 4, min(a + 32, hi), 4):
                y = w(b)
                if y >> 26 == 9 and (y >> 21) & 31 == rt:
                    code_ptrs.add(((hi16 << 16) + ((y & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF)
                    break

candidates = set()
for lo, hi in RANGES:
    for a in range(lo, hi - 8, 4):
        x0 = w(a)
        is_jr_ra = x0 == 0x03E00008
        # Other unconditional exits: `j target` (C++ this-adjusting thunks, tail calls) and `jr reg`.
        is_other_exit = (x0 >> 26) == 2 or ((x0 >> 26) == 0 and (x0 & 0x3F) == 8 and (x0 >> 21) & 31 != 31)
        if not (is_jr_ra or is_other_exit):
            continue
        b = a + 8
        while b < hi and w(b) == 0:
            b += 4
        if b < hi and b not in funcs:
            x = w(b)
            prologue = (x >> 16) == 0x27BD and (x & 0x8000)
            # After a non-return exit only trust addresses something actually points at.
            if b in code_ptrs or (prologue and is_jr_ra):
                candidates.add(b)

split = 0
for c in sorted(candidates):
    starts = sorted(funcs)
    import bisect
    i = bisect.bisect_right(starts, c) - 1
    if i < 0:
        continue
    fs = starts[i]
    fe = min(starts[i + 1] if i + 1 < len(starts) else range_of(fs)[1], range_of(fs)[1])
    if not (fs < c < fe):
        continue
    crosses = False
    for a in range(fs, fe, 4):
        x = w(a)
        t = branch_target(a, x)
        # `j` to a function entry is a tail call, not an internal branch.
        if x >> 26 == 2 and t in funcs:
            continue
        if t is not None and fs <= t < fe and (a < c) != (t < c):
            crosses = True
            break
    if not crosses:
        funcs[c] = 'fn_%08X' % c
        split += 1
print(f'indirect-only candidates {len(candidates)}, split off {split} new functions')

# A discovered start in a branch delay slot truncates its owning function.
# GoW/SotC comparison and tools/audit_delay_slots.py identified this issue.
# Leave ambiguous JR $ra padding alone, as the historical audit does.
def has_delay_slot(x):
    op = x >> 26
    return (op in (1, 2, 3, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or
            (op == 0 and (x & 0x3f) in (8, 9)) or
            (op in (0x10, 0x11, 0x12) and ((x >> 21) & 31) == 8))

bad_starts = [s for s in funcs if range_of(s - 4) and
              w(s - 4) != 0x03E00008 and has_delay_slot(w(s - 4))]
for s in bad_starts:
    del funcs[s]
print(f'Removed {len(bad_starts)} branch-delay-slot starts')

starts = sorted(funcs)
with open(args.out_map, 'w') as out:
    out.write('Name,Start,End,Size\n')
    for i, s in enumerate(starts):
        lo, hi = range_of(s)
        end = min(starts[i + 1] if i + 1 < len(starts) else hi, hi)
        out.write(f'{funcs[s]},0x{s:08X},0x{end:08X},{end - s}\n')

print(f'{len(starts)} functions: {len(ctors)} ctors, {len(new) - len(ctors)} other block-2 starts, block2={BLOCK2[0]:#x}-{BLOCK2[1]:#x}')

toml = open(args.config, encoding='utf-8').read()
toml = re.sub(r'^output = .*$', 'output = '+__import__('json').dumps(Path(args.output).resolve().as_posix()), toml, flags=re.M)
toml = re.sub(r'^ghidra_output = .*$', 'ghidra_output = '+__import__('json').dumps(Path(args.out_map).resolve().as_posix()), toml, flags=re.M)
# sceSifFreeSysMemory isn't recognised by the analyzer; stub it so module staging buffers get freed.
toml = toml.replace('stubs = [\n', 'stubs = [\n  "sceSifFreeSysMemory@0x00126760",\n', 1)
open(args.out_config, 'w', encoding='utf-8').write(toml)
