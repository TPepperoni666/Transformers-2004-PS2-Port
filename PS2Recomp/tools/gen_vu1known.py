"""Transformers port: build a private MPG window index; never commit its output.
Every aligned 1..256-pair span is indexed. ELF bytes, not captures, are authoritative.
The manifest supplies ELF offsets and names only; all SHA256s are verified.
"""
import argparse, hashlib, json, struct, ctypes
from pathlib import Path

MASK = (1 << 64) - 1
MULT = 0x9e3779b185ebca87
SEED = 0x6a09e667f3bcc909

def hash_words(words):
    h = SEED
    for w in words:
        h = ((h ^ w) * MULT) & MASK
    return h

def generate(elf, manifest, output):
    raw = Path(elf).read_bytes()
    m = json.loads(Path(manifest).read_text())
    expected='ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb'
    if hashlib.sha256(raw).hexdigest()!=expected or m['elf_sha256']!=expected:
        raise SystemExit('Refusing unsupported ELF SHA256')
    root=Path(__file__).resolve().parents[2]
    dest=Path(output).resolve()
    if dest==root or root in dest.parents:raise SystemExit('Output must be outside the source checkout')
    data = bytearray()
    entries = []
    programs = []
    for pid, p in enumerate(m['programs']):
        b = raw[p['offset']:p['offset'] + p['size']]
        if hashlib.sha256(b).hexdigest()!=p['sha256']:raise SystemExit('Program hash mismatch')
        assert b and len(b) % 8 == 0
        base = len(data)
        data.extend(b)
        words = struct.unpack('<%dQ' % (len(b)//8), b)
        programs.append(dict(id=pid, name=p['name'], bytes=len(b)))
        for start in range(len(words)):
            h = SEED
            for n, w in enumerate(words[start:start+256], 1):
                h = ((h ^ w) * MULT) & MASK
                entries.append((h, base + start*8, n, pid))
    entries.sort()
    # Retain hash collisions and duplicate spans. Runtime checks the actual bytes,
    # so no 64-bit collision can admit new code or misclassify a known span.
    out = Path(output)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open('wb') as f:
        f.write(struct.pack('<8sII', b'VU1NEW1\0', len(entries), len(data)))
        for e in entries:
            f.write(struct.pack('<QIHH', *e))
        f.write(data)
    out.with_suffix('.json').write_text(json.dumps(dict(programs=programs, windows=len(entries), bytes=out.stat().st_size), indent=2))
    print('VU1 known:', len(programs), 'programs,', len(entries), 'windows,', out.stat().st_size, 'bytes')

if __name__ == '__main__':
    if hasattr(ctypes, 'windll'):
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    a = argparse.ArgumentParser()
    a.add_argument('--elf', required=True)
    a.add_argument('--manifest', required=True)
    a.add_argument('--output', '--out', required=True)
    args = a.parse_args()
    generate(args.elf, args.manifest, args.output)
