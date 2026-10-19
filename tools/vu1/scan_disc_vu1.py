#!/usr/bin/env python3
"""scan_disc_vu1.py - disc-wide VU1 microcode scanner for Transformers (USA, SLUS-20668).

Question it answers: does any file on the disc (outside the boot ELF) carry VU1 microcode,
and if so is it a copy of one of the 66 programs the extractor identifies from the ELF?

What it does (read-only on the ISO; nothing is extracted to disk):
  * Walks the ISO9660 filesystem directly (cross-checked against an optional ISO map).
  * Scans the raw bytes of EVERY ISO file, plus every member of the four PKZIP containers
    (decompressed in memory, CRC-checked), plus any zlib/deflate stream found inside a member.
  * Detector 1 - VIF MPG: a 32-bit word whose command byte (bits 31:24, interrupt bit masked)
    is 0x4A, with plausible VU1 NUM/IMM (IMM < 2048, IMM+NUM <= 2048, NUM 0 = 256), followed by
    NUM*8 bytes that all decode as valid VU upper/lower instruction pairs.  All 8 byte phases are
    scanned because FISH (VBIN) chunks are not 4-byte aligned in the files.
  * Detector 2 - copies: any run of >= 16 consecutive 64-bit pairs (any byte phase) identical to
    a contiguous range inside one of the 66 known programs, extended to its maximal length.
  * Detector 3 - raw microcode: runs of >= 16 strictly-valid, code-shaped pairs with no MPG
    wrapper (this is how the 66 programs are stored in the ELF; the game builds MPG tags at run
    time in sub_00225300).  Calibrated on the ELF: real programs have >= 35% exact NOP words,
    every false run in ELF data had 0%.
  * Opaque check: whole-file byte entropy, 64 KiB block entropy, zlib compressibility on samples
    and magic numbers, so files a raw scan cannot see into are listed.

Outputs (offsets, hashes, counts only - no microcode or asset bytes are written):
  <out>/scan_results.json, <out>/file_inventory.tsv

Usage: pass --iso, --manifest and --out pointing to private local files.
"""
import argparse, collections, ctypes, hashlib, json, math, mmap, os, struct, sys, time, zlib
from pathlib import Path

import numpy as np

SECTOR = 2048
MIN_COPY_PAIRS = 16
MIN_RAW_PAIRS = 16
CHUNK = 16 << 20          # bytes per scan chunk for big files
OVERLAP = 64 << 10        # > max MPG payload (2 KiB) and > VU1 micro memory (16 KiB)

# ---------------------------------------------------------------------------------------------
# VU instruction validity tables.  Built from the VU1 ISA (Sony VU User's Manual instruction
# tables, as implemented in PS2Recomp/ps2xRuntime/src/lib/vu/
# ps2_vu1_upper.cpp / ps2_vu1_lower.cpp).  Special-group selectors use
# (instr & 3) | ((instr >> 4) & 0x7C), exactly as those files do.
# ---------------------------------------------------------------------------------------------
U_SPEC_OK = np.zeros(128, bool)
U_SPEC_OK[0x00:0x2B] = True                  # ADDAbc..MADDAi..MULA (0x00-0x2A)
U_SPEC_OK[[0x2C, 0x2D, 0x2E, 0x2F]] = True   # SUBA, MSUBA, OPMULA, NOP (0x2B reserved)
L1_FUNCT_OK = np.zeros(64, bool)
L1_FUNCT_OK[[0x30, 0x31, 0x32, 0x34, 0x35, 0x3C, 0x3D, 0x3E, 0x3F]] = True  # IADD ISUB IADDI IAND IOR + special
L1_SPEC_OK = np.zeros(128, bool)
L1_SPEC_OK[[0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
            0x40, 0x41, 0x42, 0x43, 0x64, 0x68, 0x69, 0x6C, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
            0x76, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E]] = True
L2_OK = np.zeros(128, bool)
L2_OK[[0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
       0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F]] = True

VIF_CMDS = {0x00: 'NOP', 0x01: 'STCYCL', 0x02: 'OFFSET', 0x03: 'BASE', 0x04: 'ITOP', 0x05: 'STMOD',
            0x06: 'MSKPATH3', 0x07: 'MARK', 0x10: 'FLUSHE', 0x11: 'FLUSH', 0x13: 'FLUSHA',
            0x14: 'MSCAL', 0x15: 'MSCALF', 0x17: 'MSCNT', 0x20: 'STMASK', 0x30: 'STROW',
            0x31: 'STCOL', 0x4A: 'MPG', 0x50: 'DIRECT', 0x51: 'DIRECTHL'}


def pair_flags(a):
    """a: uint64 array of VU pairs (lower word in bits 31:0, upper in 63:32, as stored in memory).
    Returns dict of boolean arrays."""
    L = (a & np.uint64(0xFFFFFFFF)).astype(np.uint32)
    U = (a >> np.uint64(32)).astype(np.uint32)
    op = U & 0x3F
    spec = op >= 0x3C
    sp = ((U & 3) | ((U >> 4) & 0x7C)).astype(np.intp)
    u_isa = (((U >> 25) & 3) == 0) & ((op < 0x30) | (spec & U_SPEC_OK[sp]))
    u_nop = spec & (sp == 0x2F)
    # strict (calibrated on all 19,866 pairs of the 66 known programs: 100% pass):
    #   upper NOP has no stray fields, non-NOP FMAC ops write >= 1 component, M/D/T bits clear.
    u_strict = u_isa & (~u_nop | ((U & 0x07FFFFFF) == 0x2FF)) & (u_nop | (((U >> 21) & 0xF) != 0)) \
        & (((U >> 27) & 7) == 0)
    ibit = (U >> 31) == 1
    hi = ((L >> 25) & 0x7F).astype(np.intp)
    f = (L & 0x3F).astype(np.intp)
    f2 = ((L & 3) | ((L >> 4) & 0x7C)).astype(np.intp)
    l1 = (hi == 0x40) & L1_FUNCT_OK[f] & ((f < 0x3C) | L1_SPEC_OK[f2])
    l_ok = ibit | l1 | L2_OK[hi]          # with the I bit set the lower word is a float immediate
    zero = a == 0
    valid = u_strict & l_ok & ~zero
    lenient = u_isa & l_ok & ~zero
    nopw = ((U & 0x07FFFFFF) == 0x2FF) | (~ibit & (L == 0x8000033C))
    ebit = ((U >> 30) & 1) == 1
    return dict(valid=valid, lenient=lenient, zero=zero, nop=nopw, ebit=ebit, L=L)


def cums(b):
    c = np.zeros(len(b) + 1, np.int32)
    np.cumsum(b, out=c[1:])
    return c


def runs_of(mask):
    d = np.diff(np.concatenate(([0], mask.view(np.int8), [0])).astype(np.int8))
    return np.flatnonzero(d == 1), np.flatnonzero(d == -1)


# ---------------------------------------------------------------------------------------------
# Known programs
# ---------------------------------------------------------------------------------------------
class Known:
    def __init__(self, programs):
        self.progs = programs            # list of dict(name(s), sha256, offset, size, bytes)
        self.arrs = [np.frombuffer(p['bytes'], '<u8') for p in programs]
        allp = np.concatenate(self.arrs)
        self.keys = np.unique(allp[allp != 0])
        self.HBITS = 22
        self.MULT = np.uint64(0x9E3779B97F4A7C15)
        self.table = np.zeros(1 << self.HBITS, bool)
        self.table[self._h(self.keys)] = True
        self.win = collections.defaultdict(list)
        for pi, arr in enumerate(self.arrs):
            b = arr.tobytes()
            for o in range(0, len(arr) - MIN_COPY_PAIRS + 1):
                self.win[b[o * 8:(o + MIN_COPY_PAIRS) * 8]].append((pi, o))

    def _h(self, x):
        return ((x * self.MULT) >> np.uint64(64 - self.HBITS)).astype(np.intp)

    def known_mask(self, a):
        cand = self.table[self._h(a)]
        idx = np.flatnonzero(cand)
        m = np.zeros(len(a), bool)
        if len(idx):
            m[idx] = np.isin(a[idx], self.keys)
        return m

    def relate(self, blob):
        """Relationship of a candidate byte blob (8-byte aligned code) to the known programs."""
        h = hashlib.sha256(blob).hexdigest()
        for p in self.progs:
            if p['sha256'] == h:
                return dict(relation='identical_to_program', program=p['names'][0], sha256=h)
        for p in self.progs:
            pos = p['bytes'].find(blob)
            while pos >= 0 and pos % 8:
                pos = p['bytes'].find(blob, pos + 1)
            if pos >= 0:
                return dict(relation='subset_of_program', program=p['names'][0], program_offset=pos, sha256=h)
        a = np.frombuffer(blob[:len(blob) // 8 * 8], '<u8')
        nz = a[a != 0]
        frac = float(np.isin(nz, self.keys).mean()) if len(nz) else 0.0
        return dict(relation='not_contained_in_any_program', known_pair_fraction=round(frac, 4), sha256=h)


# ---------------------------------------------------------------------------------------------
# Scanner for one byte buffer
# ---------------------------------------------------------------------------------------------
def scan_bytes(data, known, label, file_is_elf=False, max_records=300):
    """Scan one logical file.  Returns dict with mpg/copy/raw findings (offsets within `data`)."""
    res = dict(mpg_word_hits=0, mpg_plausible=0, mpg_decodes=0, mpg_decodes_lenient=0, mpg_strong=0,
               mpg_records=[], copy_segments=[], raw_runs=[], raw_runs_total=0)
    n_total = len(data)
    if n_total < 16:
        return res
    copy_seen = set()
    starts = list(range(0, n_total, CHUNK))
    for base in starts:
        core_end = min(base + CHUNK, n_total)
        buf = data[base:min(core_end + OVERLAP, n_total)]
        at_file_end = base + len(buf) >= n_total
        for ph in range(4):
            q = ph + 4
            P = {}
            for phase in (ph, q):
                n = (len(buf) - phase) // 8
                if n <= 0:
                    P[phase] = None
                    continue
                a = np.frombuffer(buf, '<u8', count=n, offset=phase)
                fl = pair_flags(a)
                fl['a'] = a
                fl['known'] = known.known_mask(a)
                fl['c_bad'] = cums(~(fl['valid'] | fl['zero']))
                fl['c_badlen'] = cums(~(fl['lenient'] | fl['zero']))
                fl['c_zero'] = cums(fl['zero'])
                fl['c_nop'] = cums(fl['nop'])
                fl['c_e'] = cums(fl['ebit'])
                fl['c_known'] = cums(fl['known'])
                P[phase] = fl
            # ---- detector 1: MPG words (L word of phase src -> payload in other phase)
            for src, dst, joff in ((ph, q, 0), (q, ph, 1)):
                S, D = P.get(src), P.get(dst)
                if S is None or D is None:
                    continue
                Lw = S['L']
                hit = ((Lw >> 24) & 0x7F) == 0x4A
                idx = np.flatnonzero(hit)
                if not len(idx):
                    continue
                offs = base + src + 8 * idx
                keep = offs < core_end
                idx, offs = idx[keep], offs[keep]
                res['mpg_word_hits'] += int(len(idx))
                w = Lw[idx]
                num = ((w >> 16) & 0xFF).astype(np.int64)
                num[num == 0] = 256
                imm = (w & 0xFFFF).astype(np.int64)
                plaus = (imm < 2048) & (imm + num <= 2048)
                idx, offs, num, imm, w = idx[plaus], offs[plaus], num[plaus], imm[plaus], w[plaus]
                res['mpg_plausible'] += int(len(idx))
                j0 = idx + joff
                j1 = j0 + num
                inb = j1 <= len(D['a'])
                trunc = ~inb
                j0c, j1c = j0[inb], j1[inb]
                bad = D['c_bad'][j1c] - D['c_bad'][j0c]
                badl = D['c_badlen'][j1c] - D['c_badlen'][j0c]
                zer = D['c_zero'][j1c] - D['c_zero'][j0c]
                nonzero = num[inb] - zer
                dec = (bad == 0) & (nonzero >= 1)
                decl = (badl == 0) & (nonzero >= 1)
                nop = D['c_nop'][j1c] - D['c_nop'][j0c]
                eb = D['c_e'][j1c] - D['c_e'][j0c]
                kn = D['c_known'][j1c] - D['c_known'][j0c]
                strong = dec & (nonzero >= 4) & (nop >= 0.15 * nonzero) & (eb <= np.maximum(2, 0.1 * nonzero))
                res['mpg_decodes'] += int(dec.sum())
                res['mpg_decodes_lenient'] += int(decl.sum())
                res['mpg_strong'] += int(strong.sum())
                sel = np.flatnonzero(dec)
                o_in, n_in, i_in, w_in = offs[inb], num[inb], imm[inb], w[inb]
                for k in sel:
                    if len(res['mpg_records']) >= max_records and not strong[k]:
                        continue
                    off = int(o_in[k]); nn = int(n_in[k])
                    blob = bytes(data[off + 4: off + 4 + nn * 8])
                    prev = data[off - 4:off] if off >= 4 else b''
                    nxt = data[off + 4 + nn * 8: off + 8 + nn * 8]
                    rec = dict(offset=off, vifcode='0x%08X' % int(w_in[k]), interrupt_bit=bool(int(w_in[k]) >> 31),
                               num_pairs=nn, load_pair_addr=int(i_in[k]), load_byte_addr=int(i_in[k]) * 8,
                               nonzero_pairs=int(nonzero[k]), nop_words=int(nop[k]), e_bits=int(eb[k]),
                               known_pairs=int(kn[k]), strong=bool(strong[k]), file_byte_phase=off % 16,
                               prev_vifcmd=VIF_CMDS.get(prev[3] & 0x7F, 'other') if len(prev) == 4 else None,
                               next_vifcmd=VIF_CMDS.get(nxt[3] & 0x7F, 'other') if len(nxt) == 4 else None)
                    rec.update(known.relate(blob))
                    res['mpg_records'].append(rec)
            # ---- detector 2 (copies) and 3 (raw code runs) per phase
            for phase in (ph, q):
                F = P.get(phase)
                if F is None:
                    continue
                a = F['a']; n = len(a)
                # copies
                allowed = F['known'] | F['zero']
                ca = cums(allowed)
                ck = F['c_known']
                if n >= MIN_COPY_PAIRS:
                    W = MIN_COPY_PAIRS
                    ok = (ca[W:] - ca[:-W] == W) & (ck[W:] - ck[:-W] >= W // 2)
                    cand = np.flatnonzero(ok)
                    for i in cand:
                        off = base + phase + 8 * int(i)
                        if off >= core_end:
                            break
                        hits = known.win.get(a[i:i + W].tobytes())
                        if not hits:
                            continue
                        for pi, o in hits:
                            diag = (phase % 8, (off - 8 * o), pi)
                            if diag in copy_seen:
                                continue
                            parr = known.arrs[pi]
                            k = W
                            while i + k < n and o + k < len(parr) and a[i + k] == parr[o + k]:
                                k += 1
                            b = 0
                            while i - b - 1 >= 0 and o - b - 1 >= 0 and a[i - b - 1] == parr[o - b - 1]:
                                b += 1
                            copy_seen.add(diag)
                            seg_off = off - 8 * b
                            seg_len = b + k
                            res['copy_segments'].append(dict(
                                offset=seg_off, pairs=seg_len, program=known.progs[pi]['names'][0],
                                program_offset=8 * (o - b), program_pairs=len(parr),
                                whole_program=(o - b == 0 and seg_len == len(parr)),
                                byte_phase_mod8=seg_off % 8))
                # raw code-shaped runs
                st, en = runs_of(F['valid'])
                ln = en - st
                big = np.flatnonzero(ln >= MIN_RAW_PAIRS)
                for r in big:
                    s, e = int(st[r]), int(en[r])
                    off = base + phase + 8 * s
                    if off >= core_end:
                        continue
                    if base > 0 and s == 0 and off >= 8:
                        # run touches the chunk start: skip it if it really began in the previous chunk
                        pv = pair_flags(np.frombuffer(bytes(data[off - 8:off]), '<u8'))['valid'][0]
                        if pv:
                            continue
                    m = e - s
                    nopc = int(F['c_nop'][e] - F['c_nop'][s])
                    ec = int(F['c_e'][e] - F['c_e'][s])
                    kc = int(F['c_known'][e] - F['c_known'][s])
                    distinct = int(len(np.unique(a[s:e])))
                    codey = nopc >= 0.15 * m and ec <= max(2, 0.1 * m) and distinct >= 4
                    if not codey:
                        continue
                    res['raw_runs_total'] += 1
                    blob = bytes(a[s:e].tobytes())
                    rec = dict(offset=off, pairs=m, nop_words=nopc, e_bits=ec, known_pairs=kc, distinct_pairs=distinct,
                               truncated_at_chunk=(e == n and not at_file_end))
                    rec.update(known.relate(blob))
                    if len(res['raw_runs']) < max_records or file_is_elf:
                        res['raw_runs'].append(rec)
    res['copy_segments'].sort(key=lambda r: r['offset'])
    res['raw_runs'].sort(key=lambda r: r['offset'])
    res['mpg_records'].sort(key=lambda r: r['offset'])
    return res


# ---------------------------------------------------------------------------------------------
# Opaque / compressed detection
# ---------------------------------------------------------------------------------------------
MAGICS = [(b'\x7fELF', 'ELF'), (b'FISH', 'FISH tagged chunk tree (VBIN)'), (b'PK\x01\x02', 'PKZIP (central dir first)'),
          (b'PK\x03\x04', 'PKZIP'), (b'\x00\x00\x01\xba', 'MPEG program stream'), (b'\x00\x00\x01\xb3', 'MPEG video ES'),
          (b'\xff\xd8\xff', 'JPEG'), (b'RESET\x00', 'IOPRP ROMDIR image'), (b'VAGp', 'VAG ADPCM'), (b'TIM2', 'TIM2'),
          (b'MThd', 'MIDI'), (b'RIFF', 'RIFF'), (b'\x1f\x8b', 'gzip'), (b'BZh', 'bzip2'), (b'Yaz0', 'Yaz0'),
          (b'\x78\x9c', 'zlib'), (b'\x78\xda', 'zlib'), (b'\x78\x01', 'zlib'), (b'\x78\x5e', 'zlib'),
          (b'BOOT2', 'SYSTEM.CNF text'), (b'\xef\xbb\xbf', 'UTF-8 text'), (b'IECS', 'IECS sound bank')]
MEDIA_KINDS = {'MPEG program stream', 'MPEG video ES', 'JPEG'}


def magic_of(b):
    for m, name in MAGICS:
        if b.startswith(m):
            return name
    head = b[:64]
    if head and all(32 <= c < 127 or c in (9, 10, 13) for c in head):
        return 'text'
    return 'unknown(%s)' % b[:4].hex()


def entropy_stats(data):
    n = len(data)
    if n == 0:
        return dict(entropy=0.0, block_hi_frac=0.0, zlib_ratio=1.0)
    arr = np.frombuffer(data, np.uint8)
    cnt = np.bincount(arr, minlength=256).astype(np.float64)
    p = cnt[cnt > 0] / n
    ent = float(-(p * np.log2(p)).sum())
    B = 65536
    hi = 0; nb = 0
    if n >= B:
        for s in range(0, n - B + 1, B):
            c = np.bincount(arr[s:s + B], minlength=256).astype(np.float64)
            pp = c[c > 0] / B
            nb += 1
            hi += float(-(pp * np.log2(pp)).sum()) >= 7.5
    # zlib compressibility on up to 16 evenly spaced 64 KiB samples
    if n <= 16 * B:
        sample = bytes(data)
    else:
        step = n // 16
        sample = b''.join(bytes(data[i * step:i * step + B]) for i in range(16))
    ratio = len(zlib.compress(sample, 1)) / max(1, len(sample))
    return dict(entropy=round(ent, 4), block_hi_frac=round(hi / nb, 4) if nb else None, zlib_ratio=round(ratio, 4))


def opaque_verdict(kind, st, size):
    if size < 512:
        return 'too_small_to_judge'
    hi = st['zlib_ratio'] >= 0.95 or st['entropy'] >= 7.6 or (st['block_hi_frac'] or 0) >= 0.5
    if not hi:
        return 'transparent'
    if kind in MEDIA_KINDS:
        return 'opaque_media'
    if kind.startswith('PKZIP'):
        return 'opaque_container_members_scanned'
    return 'opaque_unknown'


def try_inner_streams(data):
    """Look for a zlib / raw-deflate stream starting in the first 64 bytes of an opaque member.
    Probe on a 64 KiB prefix first; accept only if the full stream inflates to its end (>= 1 KiB)."""
    out = []
    n = len(data)
    for off in range(0, min(n - 2, 64)):
        for wbits in (15, -15):
            if wbits == 15 and not (data[off] == 0x78 and (data[off] * 256 + data[off + 1]) % 31 == 0):
                continue
            try:
                d = zlib.decompressobj(wbits)
                o = d.decompress(bytes(data[off:off + 65536]), 1 << 20)
                if len(o) < 1024 and not d.eof:
                    continue
                d = zlib.decompressobj(wbits)
                o = d.decompress(bytes(data[off:]), 256 << 20)
                if len(o) >= 1024 and d.eof:
                    out.append((off, wbits, o))
                    return out
            except zlib.error:
                pass
    return out


# ---------------------------------------------------------------------------------------------
# ISO9660 and ZIP readers
# ---------------------------------------------------------------------------------------------
def iso_files(mm):
    pvd = mm[16 * SECTOR:17 * SECTOR]
    if pvd[0] != 1 or pvd[1:6] != b'CD001':
        raise ValueError('no ISO9660 PVD at sector 16')
    bs = struct.unpack_from('<H', pvd, 128)[0]
    vol_blocks = struct.unpack_from('<I', pvd, 80)[0]
    root = pvd[156:156 + 34]
    out = []

    def walk(lba, size, path):
        d = mm[lba * bs:lba * bs + size]
        p = 0
        while p < len(d):
            ln = d[p]
            if ln == 0:
                p = (p // bs + 1) * bs
                continue
            ext = struct.unpack_from('<I', d, p + 2)[0]
            dl = struct.unpack_from('<I', d, p + 10)[0]
            flags = d[p + 25]
            nl = d[p + 32]
            name = d[p + 33:p + 33 + nl]
            if name not in (b'\x00', b'\x01'):
                nm = name.decode('latin1').split(';')[0]
                if flags & 2:
                    walk(ext, dl, path + nm + '/')
                else:
                    out.append(dict(path=path + nm, lsn=ext, size=dl, multi_extent=bool(flags & 0x80)))
            p += ln

    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '/')
    return out, bs, vol_blocks


def zip_members(b):
    """Central directory at the front (this game's layout) or at the end; CRC-checked."""
    out = []
    if b[:4] == b'PK\x01\x02':
        p = 0
    else:
        e = b.rfind(b'PK\x05\x06')
        p = struct.unpack_from('<I', b, e + 16)[0]
    covered = []
    cd_start = p
    while b[p:p + 4] == b'PK\x01\x02':
        method = struct.unpack_from('<H', b, p + 10)[0]
        crc, cs, us = struct.unpack_from('<III', b, p + 16)
        nl, xl, cl = struct.unpack_from('<HHH', b, p + 28)
        lh = struct.unpack_from('<I', b, p + 42)[0]
        name = b[p + 46:p + 46 + nl].decode('latin1').replace('\\', '/')
        if b[lh:lh + 4] != b'PK\x03\x04':
            raise ValueError('bad local header ' + name)
        ln, lx = struct.unpack_from('<HH', b, lh + 26)
        base = lh + 30 + ln + lx
        raw = b[base:base + cs]
        data = zlib.decompress(raw, -15) if method == 8 else bytes(raw) if method == 0 else None
        if data is None:
            raise ValueError('unsupported method %d in %s' % (method, name))
        ok = len(data) == us and (zlib.crc32(data) & 0xFFFFFFFF) == crc
        out.append(dict(name=name, method=method, csize=cs, usize=us, crc_ok=ok, local_header=lh, data=data))
        covered.append((lh, base + cs))
        p += 46 + nl + xl + cl
    covered.append((cd_start, p))
    e = b.find(b'PK\x05\x06', p - 4 if p >= 4 else 0)
    if e >= 0:
        covered.append((e, e + 22 + struct.unpack_from('<H', b, e + 20)[0]))
    covered.sort()
    gaps = []
    pos = 0
    for s, t in covered:
        if s > pos:
            gaps.append((pos, s))
        pos = max(pos, t)
    if pos < len(b):
        gaps.append((pos, len(b)))
    return out, gaps


# ---------------------------------------------------------------------------------------------
def selftest(known, seed=1234):
    """Positive controls: plant known programs in random noise at awkward offsets and check every
    detector fires with the right relation; plant noise-only buffers and check nothing fires."""
    rng = np.random.default_rng(seed)
    out = []
    progs = [p for p in known.progs if 16 <= len(p['bytes']) // 8 <= 256][:6]
    for k, p in enumerate(progs):
        n = len(p['bytes']) // 8
        noise = rng.integers(0, 256, 200003, dtype=np.uint8).tobytes()
        off = 1001 + 3 * k                       # deliberately not 4- or 8-aligned
        vif = struct.pack('<I', (0x4A << 24) | ((n & 0xFF) << 16) | (k * 8))
        flushe = struct.pack('<I', 0x10 << 24)
        sub = p['bytes'][8:8 + 16 * 8]            # a 16-pair slice, raw (no MPG), for the copy detector
        buf = noise[:off] + flushe + vif + p['bytes'] + noise[off:off + 5000] + sub + noise[off + 5000:]
        r = scan_bytes(buf, known, 'selftest')
        m = [x for x in r['mpg_records'] if x['offset'] == off + 4]
        out.append(dict(program=p['names'][0], pairs=n, mpg_found=bool(m),
                        mpg_relation=m[0]['relation'] if m else None, mpg_strong=m[0]['strong'] if m else None,
                        copy_whole=any(c['whole_program'] for c in r['copy_segments']),
                        copy_subslice=any(c['pairs'] == 16 and c['program_offset'] == 8 for c in r['copy_segments']),
                        raw_runs=r['raw_runs_total'], mpg_decodes_total=r['mpg_decodes'], mpg_strong_total=r['mpg_strong']))
    noise = rng.integers(0, 256, 8 << 20, dtype=np.uint8).tobytes()
    r = scan_bytes(noise, known, 'noise')
    out.append(dict(program='(8 MiB uniform noise)', mpg_plausible=r['mpg_plausible'], mpg_decodes=r['mpg_decodes'],
                    mpg_strong=r['mpg_strong'], copies=len(r['copy_segments']), raw_runs=r['raw_runs_total']))
    return out


LEVEL_RECORDED = {'01_AMAZON_1': True, '02_ANTARCTICA': True, '03_AMAZON_2': True}


def level_of(path):
    P = path.upper().strip('/').split('/')
    if 'EPISODES' in P:
        i = P.index('EPISODES')
        if i + 1 < len(P) - 1:
            return P[i + 1]
    if 'AUDIOCUE' in P:
        i = P.index('AUDIOCUE')
        return 'audiocue:' + P[i + 1]
    if len(P) >= 2 and P[0] == 'FMV':
        return 'fmv:' + ('/'.join(P[1:-1]) or 'root')
    if P[0] == 'SOUND' and P[-1].endswith('.TLA'):
        return 'sound:' + P[-1][:-4]
    if P[0] in ('LEVELS',):
        return 'levels:' + (P[1] if len(P) > 2 else 'root')
    return P[0] if len(P) > 1 else 'root'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--iso', required=True)
    ap.add_argument('--root', default='.')
    ap.add_argument('--manifest', required=True)
    ap.add_argument('--programs-dir', default='missing-programs')
    ap.add_argument('--isomap', default='missing-isomap.tsv')
    ap.add_argument('--out', required=True)
    ap.add_argument('--limit-files', type=int, default=0, help='debug: stop after N ISO files')
    ap.add_argument('--selftest', action='store_true', help='run positive/negative controls only (writes selftest.json)')
    args = ap.parse_args()
    root = Path(args.root)
    out = root / args.out
    checkout=Path(__file__).resolve().parents[2]
    out=out.resolve()
    if out==checkout or checkout in out.parents:raise SystemExit('Output must be outside the source checkout')
    with open(args.iso,'rb') as verify:
        digest=hashlib.file_digest(verify,'sha256').hexdigest()
    if digest!='89fc6ee694303bac0993b72bbdd89be8cc58d25af831b7072b6ec67fad2b37cf':raise SystemExit('Refusing unsupported disc SHA256')
    if hasattr(ctypes, 'windll'):
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)  # BelowNormal
    t0 = time.monotonic()

    fh = open(args.iso, 'rb')
    mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
    iso_size = len(mm)
    files, bs, vol_blocks = iso_files(mm)
    files.sort(key=lambda f: f['lsn'])
    print('ISO: %d bytes, volume %d blocks, %d files' % (iso_size, vol_blocks, len(files)), flush=True)

    # cross-check with isomap.tsv
    isomap = {}
    ip = root / args.isomap
    if ip.exists():
        for line in ip.read_text().splitlines():
            if line.startswith('#') or not line.strip():
                continue
            lsn, sec, path = line.split('\t')[:3]
            isomap[path] = (int(lsn), int(sec))
    map_check = dict(isomap_entries=len(isomap), iso_entries=len(files), mismatches=[])
    for f in files:
        m = isomap.get(f['path'])
        sec = (f['size'] + SECTOR - 1) // SECTOR
        if m is None or m[0] != f['lsn'] or m[1] != sec:
            map_check['mismatches'].append(dict(path=f['path'], iso=(f['lsn'], sec), isomap=m))
        f['end_beyond_image'] = f['lsn'] * SECTOR + f['size'] > iso_size

    # known programs, read from the ELF inside the ISO and verified against the manifest
    man = json.loads((root / args.manifest).read_text())
    elf_f = next(f for f in files if f['path'].upper() == '/SLUS_206.68')
    elf = bytes(mm[elf_f['lsn'] * SECTOR: elf_f['lsn'] * SECTOR + elf_f['size']])
    elf_sha = hashlib.sha256(elf).hexdigest()
    if elf_sha!='ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb' or man['elf_sha256']!=elf_sha:raise SystemExit('Unsupported ELF identity')
    out.mkdir(parents=True,exist_ok=True)
    progs = {}
    for p in man['programs']:
        b = elf[p['offset']:p['offset'] + p['size']]
        if hashlib.sha256(b).hexdigest() != p['sha256']:
            raise SystemExit('program %s does not match ELF on ISO' % p['name'])
        if p['sha256'] not in progs:
            progs[p['sha256']] = dict(names=[p['name']], sha256=p['sha256'], offset=p['offset'], size=p['size'], bytes=b)
        elif p['name'] not in progs[p['sha256']]['names']:
            progs[p['sha256']]['names'].append(p['name'])
    progs = sorted(progs.values(), key=lambda p: p['offset'])
    pd = root / args.programs_dir
    pdir_check = None
    if pd.exists():
        pdir_check = sum((pd / (p['sha256'] + '.bin')).exists() and (pd / (p['sha256'] + '.bin')).read_bytes() == p['bytes'] for p in progs)
    known = Known(progs)
    if args.selftest:
        st = selftest(known)
        for row in st:
            print(json.dumps(row))
        (out / 'selftest.json').write_text(json.dumps(st, indent=1))
        return
    print('known programs: %d unique, %d pairs, %d distinct non-zero pairs, %d 16-pair windows; ELF sha256 %s (manifest %s)'
          % (len(progs), sum(len(a) for a in known.arrs), len(known.keys), len(known.win), elf_sha[:16], man['elf_sha256'][:16]), flush=True)

    inventory = []
    findings = dict(mpg=[], copies=[], raw=[])
    elf_result = None

    def record(path, container, data, level, is_elf=False):
        nonlocal elf_result
        ts = time.monotonic()
        r = scan_bytes(data, known, path, file_is_elf=is_elf)
        kind = magic_of(bytes(data[:64]))
        st = entropy_stats(data)
        verdict = opaque_verdict(kind, st, len(data))
        row = dict(path=path, container=container, size=len(data), level=level, magic=kind, **st, opaque=verdict,
                   mpg_word_hits=r['mpg_word_hits'], mpg_plausible=r['mpg_plausible'], mpg_decodes=r['mpg_decodes'],
                   mpg_decodes_lenient=r['mpg_decodes_lenient'], mpg_strong=r['mpg_strong'],
                   copy_segments=len(r['copy_segments']), raw_code_runs=r['raw_runs_total'],
                   scan_seconds=round(time.monotonic() - ts, 3))
        inventory.append(row)
        if is_elf:
            elf_result = r
        else:
            for x in r['mpg_records']:
                findings['mpg'].append(dict(path=path, **x))
            for x in r['copy_segments']:
                findings['copies'].append(dict(path=path, **x))
            for x in r['raw_runs']:
                findings['raw'].append(dict(path=path, **x))
        return row

    zip_info = []
    nested = []
    for k, f in enumerate(files):
        if args.limit_files and k >= args.limit_files:
            break
        start = f['lsn'] * SECTOR
        data = memoryview(mm)[start:min(start + f['size'], iso_size)]
        is_elf = f is elf_f
        row = record(f['path'], 'iso', data, level_of(f['path']), is_elf)
        if f['size'] >= (8 << 20) or is_elf:
            print('  %-40s %10d  mpg %d/%d/%d strong %d  copies %d raw %d  %.1fs' % (
                f['path'], f['size'], row['mpg_word_hits'], row['mpg_plausible'], row['mpg_decodes'], row['mpg_strong'],
                row['copy_segments'], row['raw_code_runs'], row['scan_seconds']), flush=True)
        if f['path'].upper().endswith('.ZIP'):
            zb = bytes(data)
            members, gaps = zip_members(zb)
            zi = dict(path=f['path'], members=len(members), crc_ok=sum(m['crc_ok'] for m in members),
                      stored=sum(m['method'] == 0 for m in members), deflated=sum(m['method'] == 8 for m in members),
                      uncompressed_bytes=sum(m['usize'] for m in members),
                      uncovered_gaps=[dict(offset=s, size=t - s) for s, t in gaps])
            zip_info.append(zi)
            for g0, g1 in gaps:
                record('%s@gap_%d' % (f['path'], g0), f['path'] + ':gap', zb[g0:g1], 'zip_gap')
            for m in members:
                mp = f['path'] + '!' + m['name']
                mrow = record(mp, f['path'], m['data'], level_of(m['name']))
                if mrow['opaque'] == 'opaque_unknown':
                    for off, wb, inner in try_inner_streams(m['data']):
                        nested.append(dict(path=mp, offset=off, wbits=wb, size=len(inner)))
                        record('%s#inflate@%d' % (mp, off), mp, inner, level_of(m['name']))
            print('  %s: %d members (%d CRC ok), %d bytes, gaps %s' % (
                f['path'], zi['members'], zi['crc_ok'], zi['uncompressed_bytes'], [(g['offset'], g['size']) for g in zi['uncovered_gaps']]), flush=True)
            del zb, members

    # ----- sanity on the ELF
    elf_ranges = [(p['offset'], p['offset'] + p['size'], p['names'][0]) for p in progs]

    def in_prog(off):
        return any(s <= off < e for s, e, _ in elf_ranges)

    elf_summary = None
    if elf_result is not None:
        raw_pairs = sum(r['pairs'] for r in elf_result['raw_runs'])
        uncovered = []
        for r in elf_result['raw_runs']:
            unc = [r['offset'] + 8 * i for i in range(r['pairs']) if not in_prog(r['offset'] + 8 * i)]
            if unc:
                uncovered.append(dict(run_offset=r['offset'], run_pairs=r['pairs'], uncovered_pairs=len(unc),
                                      first_uncovered=unc[0], last_uncovered=unc[-1], relation=r['relation']))
        whole = sorted({c['program'] for c in elf_result['copy_segments'] if c['whole_program']})
        covered_progs = set()
        for c in elf_result['copy_segments']:
            covered_progs.add(c['program'])
        elf_summary = dict(
            sha256=elf_sha, matches_manifest=elf_sha == man['elf_sha256'],
            mpg_word_hits=elf_result['mpg_word_hits'], mpg_plausible=elf_result['mpg_plausible'],
            mpg_decodes=elf_result['mpg_decodes'], mpg_strong=elf_result['mpg_strong'],
            mpg_records=elf_result['mpg_records'],
            copy_segments=len(elf_result['copy_segments']),
            copy_segments_at_program_home=sum(1 for c in elf_result['copy_segments'] if in_prog(c['offset'])),
            programs_found_whole_by_copy_scan=len(whole),
            raw_code_runs=len(elf_result['raw_runs']), raw_code_pairs=raw_pairs,
            raw_runs_outside_program_ranges=uncovered,
            raw_runs=elf_result['raw_runs'],
            copy_segments_away_from_home=[c for c in elf_result['copy_segments'] if not in_prog(c['offset'])])

    # ----- summaries
    out_mpg_strong = [x for x in findings['mpg'] if x['strong']]
    rel_counts = collections.Counter(x['relation'] for x in findings['mpg'])
    opaque = [r for r in inventory if r['opaque'] in ('opaque_unknown', 'opaque_media', 'opaque_container_members_scanned')]
    by_ext = collections.defaultdict(lambda: collections.Counter())
    for r in inventory:
        ext = r['path'].rsplit('.', 1)[-1].upper() if '.' in r['path'].rsplit('/', 1)[-1] else '(none)'
        by_ext[ext][r['opaque']] += 1
    levels = sorted({r['level'] for r in inventory if r['container'] != 'iso' and r['path'].upper().find('EPISODES') >= 0})
    summary = dict(
        elapsed_seconds=round(time.monotonic() - t0, 1),
        iso=dict(path=args.iso, size=iso_size, volume_blocks=vol_blocks, files=len(files),
                 files_ending_beyond_image=[f['path'] for f in files if f['end_beyond_image']]),
        isomap_check=map_check,
        known_programs=dict(unique=len(progs), pairs=int(sum(len(a) for a in known.arrs)),
                            distinct_nonzero_pairs=int(len(known.keys)), programs_dir_byte_identical=pdir_check,
                            elf_sha256=elf_sha, manifest_elf_sha256=man['elf_sha256']),
        logical_files_scanned=len(inventory),
        bytes_scanned=int(sum(r['size'] for r in inventory)),
        zip_containers=zip_info, nested_streams=nested,
        outside_elf=dict(
            mpg_word_hits=int(sum(r['mpg_word_hits'] for r in inventory if r['path'] != elf_f['path'])),
            mpg_plausible=int(sum(r['mpg_plausible'] for r in inventory if r['path'] != elf_f['path'])),
            mpg_decodes=int(sum(r['mpg_decodes'] for r in inventory if r['path'] != elf_f['path'])),
            mpg_decodes_lenient=int(sum(r['mpg_decodes_lenient'] for r in inventory if r['path'] != elf_f['path'])),
            mpg_strong=len(out_mpg_strong),
            mpg_relation_counts=dict(rel_counts),
            copy_segments=len(findings['copies']),
            raw_code_runs=len(findings['raw'])),
        elf=elf_summary,
        level_dirs=levels,
        opaque_by_extension={k: dict(v) for k, v in sorted(by_ext.items())},
    )
    result = dict(format_version=1, tool='tools/vu1/scan_disc_vu1.py', summary=summary,
                  findings_outside_elf=findings, opaque_files=opaque, inventory=inventory,
                  rules=dict(
                      mpg='cmd byte &0x7F == 0x4A (0xCA = interrupt bit, accepted and flagged); NUM 0=256; IMM<2048 and '
                          'IMM+NUM<=2048; every payload pair strictly valid or all-zero; >=1 non-zero pair',
                      mpg_strong='decodes, >=4 non-zero pairs, NOP words >= 15% of non-zero pairs, E bits <= max(2, 10%)',
                      strict_pair='upper: ISA op table, bits 26:25 == 0, NOP exactly 0x2FF in bits 26:0, non-NOP dest != 0, '
                                  'M/D/T bits clear; lower: ISA table (lower1 bits 31:25 == 0x40) unless upper I bit set',
                      copies='>=16 consecutive pairs, any byte phase, identical to a contiguous range of a known program, '
                             'extended to maximal length; windows need >=8 non-zero known pairs',
                      raw='>=16 consecutive strictly valid non-zero pairs, NOP words >= 15%, E bits <= max(2,10%), >=4 distinct',
                      opaque='zlib(level 1) ratio >= 0.95 on samples, or entropy >= 7.6 bits/byte, or >= 50% of 64 KiB '
                             'blocks >= 7.5 bits/byte; files < 512 bytes not judged'))
    (out / 'scan_results.json').write_text(json.dumps(result, indent=1))
    cols = ['path', 'container', 'size', 'level', 'magic', 'entropy', 'block_hi_frac', 'zlib_ratio', 'opaque',
            'mpg_word_hits', 'mpg_plausible', 'mpg_decodes', 'mpg_decodes_lenient', 'mpg_strong', 'copy_segments',
            'raw_code_runs']
    with open(out / 'file_inventory.tsv', 'w', encoding='utf-8') as fo:
        fo.write('\t'.join(cols) + '\n')
        for r in inventory:
            fo.write('\t'.join(str(r[c]) for c in cols) + '\n')
    s = summary
    print('\n=== SUMMARY ===')
    print('scanned %d logical files, %.1f MB, %.1fs' % (s['logical_files_scanned'], s['bytes_scanned'] / 1e6, s['elapsed_seconds']))
    print('isomap mismatches: %d; files ending beyond image: %s' % (len(map_check['mismatches']), s['iso']['files_ending_beyond_image']))
    print('outside ELF:', json.dumps(s['outside_elf']))
    if elf_summary:
        print('ELF: mpg hits %d plausible %d decodes %d strong %d | copy segs %d (at home %d, away %d) whole progs %d | raw runs %d (%d pairs), runs outside program ranges %d'
              % (elf_summary['mpg_word_hits'], elf_summary['mpg_plausible'], elf_summary['mpg_decodes'], elf_summary['mpg_strong'],
                 elf_summary['copy_segments'], elf_summary['copy_segments_at_program_home'], len(elf_summary['copy_segments_away_from_home']),
                 elf_summary['programs_found_whole_by_copy_scan'], elf_summary['raw_code_runs'], elf_summary['raw_code_pairs'],
                 len(elf_summary['raw_runs_outside_program_ranges'])))
    print('opaque by verdict:', collections.Counter(r['opaque'] for r in inventory))
    print('wrote', out / 'scan_results.json', 'and', out / 'file_inventory.tsv')


if __name__ == '__main__':
    main()
