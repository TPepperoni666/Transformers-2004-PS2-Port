#!/usr/bin/env python3
"""VU micro-instruction disassembler (EE/VU User's Manual encoding, same tables as PCSX2 VUops / Dobie).

usage: python vudis.py <dumpfile|codefile> [start_hex] [count] [--follow]
  dumpfile = vu1dump_NN_{pre,post}.bin (16-byte header + 16K code + 16K data + VU1State)
"""
import struct, sys

XYZW = 'xyzw'


def dest(i):
    d = (i >> 21) & 0xF
    return ''.join(c for b, c in zip((8, 4, 2, 1), XYZW) if d & b)


def bcs(i):
    return XYZW[i & 3]


def ft(i): return (i >> 16) & 0x1F
def fs(i): return (i >> 11) & 0x1F
def fd(i): return (i >> 6) & 0x1F


def imm11(i):
    v = i & 0x7FF
    return v - 0x800 if v & 0x400 else v


def imm15(i):
    v = (i & 0x7FF) | (((i >> 21) & 0xF) << 11)
    return v


def imm5(i):
    v = (i >> 6) & 0x1F
    return v - 32 if v & 0x10 else v


def imm12(i):
    return (((i >> 21) & 1) << 11) | (i & 0x7FF)


UPPER_BC = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']
UPPER_MISC = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
              0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
              0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
UPPER_SPEC_BC = ['ADDA', 'SUBA', 'MADDA', 'MSUBA']
UPPER_SPEC = {0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP', 0x20: 'ADDAq', 0x21: 'MADDAq', 0x22: 'ADDAi',
              0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi', 0x27: 'MSUBAi', 0x28: 'ADDA',
              0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA', 0x2E: 'OPMULA', 0x2F: 'NOP'}


def dis_upper(i):
    flags = ''.join(c for b, c in ((31, 'I'), (30, 'E'), (29, 'M'), (28, 'D'), (27, 'T')) if (i >> b) & 1)
    op = i & 0x3F
    d = dest(i)
    if op < 0x1C:
        name = UPPER_BC[op >> 2] + bcs(op)
        s = f'{name}.{d} vf{fd(i)}, vf{fs(i)}, vf{ft(i)}{bcs(op)}'
    elif op < 0x30:
        name = UPPER_MISC[op]
        if op in (0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F):
            s = f'{name}.{d} vf{fd(i)}, vf{fs(i)}, vf{ft(i)}'
        else:
            s = f'{name}.{d} vf{fd(i)}, vf{fs(i)}, {name[-1]}'
    elif op < 0x3C:
        s = f'??upper {i:08x}'
    else:
        sp = (i & 3) | ((i >> 4) & 0x7C)
        if sp < 0x10:
            s = f'{UPPER_SPEC_BC[sp >> 2]}{bcs(sp)}.{d} ACC, vf{fs(i)}, vf{ft(i)}{bcs(sp)}'
        elif sp < 0x14:
            s = f'ITOF{[0, 4, 12, 15][sp & 3]}.{d} vf{ft(i)}, vf{fs(i)}'
        elif sp < 0x18:
            s = f'FTOI{[0, 4, 12, 15][sp & 3]}.{d} vf{ft(i)}, vf{fs(i)}'
        elif sp < 0x1C:
            s = f'MULA{bcs(sp)}.{d} ACC, vf{fs(i)}, vf{ft(i)}{bcs(sp)}'
        elif sp in UPPER_SPEC:
            n = UPPER_SPEC[sp]
            if n == 'NOP':
                s = 'NOP'
            elif n == 'ABS':
                s = f'ABS.{d} vf{ft(i)}, vf{fs(i)}'
            elif n == 'CLIP':
                s = f'CLIPw.xyz vf{fs(i)}, vf{ft(i)}w'
            elif n[-1] in 'qi' and n not in ('MULA', 'MADDA'):
                s = f'{n}.{d} ACC, vf{fs(i)}, {n[-1]}'
            else:
                s = f'{n}.{d} ACC, vf{fs(i)}, vf{ft(i)}'
        else:
            s = f'??upperspec {i:08x}'
    return s, flags


def vit(i): return (i >> 16) & 0xF
def vis(i): return (i >> 11) & 0xF
def vid(i): return (i >> 6) & 0xF


FSF = lambda i: XYZW[(i >> 21) & 3]
FTF = lambda i: XYZW[(i >> 23) & 3]

BRANCH = {0x20: 'B', 0x21: 'BAL', 0x28: 'IBEQ', 0x29: 'IBNE', 0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}


def dis_lower(i, pc):
    """returns (text, branch_target or None, kind) kind in {None,'b','cond','jr','end'}"""
    if i == 0x8000033C:
        return 'NOP', None, None
    op = (i >> 25) & 0x7F
    d = dest(i)
    if op == 0x00:
        return f'LQ.{d} vf{ft(i)}, {imm11(i)}(vi{vis(i)})', None, None
    if op == 0x01:
        return f'SQ.{d} vf{fs(i)}, {imm11(i)}(vi{vit(i)})', None, None
    if op == 0x04:
        return f'ILW.{d} vi{vit(i)}, {imm11(i)}(vi{vis(i)})', None, None
    if op == 0x05:
        return f'ISW.{d} vi{vit(i)}, {imm11(i)}(vi{vis(i)})', None, None
    if op == 0x08:
        return f'IADDIU vi{vit(i)}, vi{vis(i)}, 0x{imm15(i):x}', None, None
    if op == 0x09:
        return f'ISUBIU vi{vit(i)}, vi{vis(i)}, 0x{imm15(i):x}', None, None
    if op in (0x10, 0x11, 0x12, 0x13):
        return f'{["FCEQ", "FCSET", "FCAND", "FCOR"][op - 0x10]} vi1, 0x{i & 0xFFFFFF:06x}', None, None
    if op in (0x14, 0x15, 0x16, 0x17):
        return f'{["FSEQ", "FSSET", "FSAND", "FSOR"][op - 0x14]} vi{vit(i)}, 0x{imm12(i):03x}', None, None
    if op in (0x18, 0x1A, 0x1B):
        return f'{ {0x18: "FMEQ", 0x1A: "FMAND", 0x1B: "FMOR"}[op]} vi{vit(i)}, vi{vis(i)}', None, None
    if op == 0x1C:
        return f'FCGET vi{vit(i)}', None, None
    if op in BRANCH:
        tgt = (pc + 8 + imm11(i) * 8) & 0x3FFF
        n = BRANCH[op]
        if op == 0x20:
            return f'B 0x{tgt:04x}', tgt, 'b'
        if op == 0x21:
            return f'BAL vi{vit(i)}, 0x{tgt:04x}', tgt, 'bal'
        if op in (0x28, 0x29):
            return f'{n} vi{vit(i)}, vi{vis(i)}, 0x{tgt:04x}', tgt, 'cond'
        return f'{n} vi{vis(i)}, 0x{tgt:04x}', tgt, 'cond'
    if op == 0x24:
        return f'JR vi{vis(i)}', None, 'jr'
    if op == 0x25:
        return f'JALR vi{vit(i)}, vi{vis(i)}', None, 'jr'
    if op == 0x40:
        f = i & 0x3F
        if f == 0x30: return f'IADD vi{vid(i)}, vi{vis(i)}, vi{vit(i)}', None, None
        if f == 0x31: return f'ISUB vi{vid(i)}, vi{vis(i)}, vi{vit(i)}', None, None
        if f == 0x32: return f'IADDI vi{vit(i)}, vi{vis(i)}, {imm5(i)}', None, None
        if f == 0x34: return f'IAND vi{vid(i)}, vi{vis(i)}, vi{vit(i)}', None, None
        if f == 0x35: return f'IOR vi{vid(i)}, vi{vis(i)}, vi{vit(i)}', None, None
        if f >= 0x3C:
            f2 = (i & 3) | ((i >> 4) & 0x7C)
            T = {
                0x30: lambda: f'MOVE.{d} vf{ft(i)}, vf{fs(i)}',
                0x31: lambda: f'MR32.{d} vf{ft(i)}, vf{fs(i)}',
                0x34: lambda: f'LQI.{d} vf{ft(i)}, (vi{vis(i)}++)',
                0x35: lambda: f'SQI.{d} vf{fs(i)}, (vi{vit(i)}++)',
                0x36: lambda: f'LQD.{d} vf{ft(i)}, (--vi{vis(i)})',
                0x37: lambda: f'SQD.{d} vf{fs(i)}, (--vi{vit(i)})',
                0x38: lambda: f'DIV Q, vf{fs(i)}{FSF(i)}, vf{ft(i)}{FTF(i)}',
                0x39: lambda: f'SQRT Q, vf{ft(i)}{FTF(i)}',
                0x3A: lambda: f'RSQRT Q, vf{fs(i)}{FSF(i)}, vf{ft(i)}{FTF(i)}',
                0x3B: lambda: 'WAITQ',
                0x3C: lambda: f'MTIR vi{vit(i)}, vf{fs(i)}{FSF(i)}',
                0x3D: lambda: f'MFIR.{d} vf{ft(i)}, vi{vis(i)}',
                0x3E: lambda: f'ILWR.{d} vi{vit(i)}, (vi{vis(i)})',
                0x3F: lambda: f'ISWR.{d} vi{vit(i)}, (vi{vis(i)})',
                0x40: lambda: f'RNEXT.{d} vf{ft(i)}, R',
                0x41: lambda: f'RGET.{d} vf{ft(i)}, R',
                0x42: lambda: f'RINIT R, vf{fs(i)}{FSF(i)}',
                0x43: lambda: f'RXOR R, vf{fs(i)}{FSF(i)}',
                0x64: lambda: f'MFP.{d} vf{ft(i)}, P',
                0x68: lambda: f'XTOP vi{vit(i)}',
                0x69: lambda: f'XITOP vi{vit(i)}',
                0x6C: lambda: f'XGKICK vi{vis(i)}',
                0x70: lambda: f'ESADD P, vf{fs(i)}',
                0x71: lambda: f'ERSADD P, vf{fs(i)}',
                0x72: lambda: f'ELENG P, vf{fs(i)}',
                0x73: lambda: f'ERLENG P, vf{fs(i)}',
                0x74: lambda: f'EATANxy P, vf{fs(i)}',
                0x75: lambda: f'EATANxz P, vf{fs(i)}',
                0x76: lambda: f'ESUM P, vf{fs(i)}',
                0x77: lambda: f'ERSQRT P, vf{fs(i)}{FSF(i)}',
                0x78: lambda: f'ESQRT P, vf{fs(i)}{FSF(i)}',
                0x79: lambda: f'ESIN P, vf{fs(i)}{FSF(i)}',
                0x7A: lambda: f'ERCPR P, vf{fs(i)}{FSF(i)}',
                0x7B: lambda: 'WAITP',
                0x7C: lambda: f'EATAN P, vf{fs(i)}{FSF(i)}',
                0x7D: lambda: f'EEXP P, vf{fs(i)}{FSF(i)}',
            }
            if f2 in T:
                return T[f2](), None, None
    return f'??lower {i:08x}', None, None


def load_code(path):
    d = open(path, 'rb').read()
    if len(d) >= 16 + 32768 and struct.unpack_from('<I', d, 12)[0] in (664, 0x298):
        return d[16:16 + 16384], d
    return d, None


def line(code, pc):
    lo, up = struct.unpack_from('<II', code, pc)
    u, flags = dis_upper(up)
    if up & 0x80000000:
        f = struct.unpack('<f', struct.pack('<I', lo))[0]
        l, tgt, kind = f'LOI {f!r} (0x{lo:08x})', None, None
    else:
        l, tgt, kind = dis_lower(lo, pc)
    return f'{pc:04x}: {lo:08x} {up:08x}  {flags:3s} {u:40s} | {l}', tgt, kind, up


def linear(code, start, count):
    for k in range(count):
        pc = start + 8 * k
        if pc + 8 > len(code):
            break
        print(line(code, pc)[0])


def follow(code, start):
    """Print reachable code from start (static: both sides of conditional branches) until E bit."""
    todo = [start]
    seen = set()
    blocks = []
    while todo:
        pc = todo.pop()
        if pc in seen:
            continue
        blk = []
        while pc not in seen and pc + 8 <= len(code):
            seen.add(pc)
            txt, tgt, kind, up = line(code, pc)
            blk.append(txt)
            if up & 0x40000000:  # E bit: delay slot then stop
                blk.append(line(code, pc + 8)[0] + '   ; <- E delay slot')
                seen.add(pc + 8)
                break
            if kind in ('b', 'bal', 'cond'):
                todo.append(tgt)
                if kind == 'bal':
                    todo.append(pc + 16)
                blk.append(line(code, pc + 8)[0] + '   ; delay slot')
                seen.add(pc + 8)
                if kind == 'cond':
                    todo.append(pc + 16)
                break
            if kind == 'jr':
                blk.append(line(code, pc + 8)[0] + '   ; delay slot')
                seen.add(pc + 8)
                break
            pc += 8
        blocks.append(blk)
    for b in sorted(blocks, key=lambda b: b[0]):
        print('\n'.join(b))
        print('-' * 20)


if __name__ == '__main__':
    code, full = load_code(sys.argv[1])
    start = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0
    if '--follow' in sys.argv:
        follow(code, start)
    else:
        cnt = int(sys.argv[3]) if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else 64
        linear(code, start, cnt)
