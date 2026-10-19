"""Require byte-identical products from two fresh generation runs.
Use before/after directories when changing generator comments. No normalization:
address annotations and generated comments are part of the reproducibility gate.
"""
import argparse, hashlib, sys
from pathlib import Path

def hashes(root):
    return {p.relative_to(root).as_posix():hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}

def compare(before,after):
    left,right=hashes(before),hashes(after)
    if not left or left!=right: raise ValueError('Generation differs (or empty): '+', '.join(sorted(k for k in left.keys()|right.keys() if left.get(k)!=right.get(k))))

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('before',type=Path);a.add_argument('after',type=Path);args=a.parse_args()
    try: compare(args.before,args.after)
    except ValueError as e: sys.exit(str(e))
    print('Byte-identical generation inventory')
