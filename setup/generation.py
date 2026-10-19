"""Private generation checkpoints and build freshness checks (stdlib only)."""
from pathlib import Path
import hashlib, json, os, sys, uuid
ROOT=Path(__file__).resolve().parents[1]

def sha(path):
    h=hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda:f.read(1024*1024),b''): h.update(block)
    return h.hexdigest()

def dependencies():
    paths=[]
    for folder in ('setup','tools','PS2Recomp/ps2xAnalyzer','PS2Recomp/ps2xRecomp','PS2Recomp/ps2xUtils','PS2Recomp/ps2xCore','PS2Recomp/tools','PS2Recomp/ps2xRuntime/include','PS2Recomp/ps2xRuntime/cmake'):
        paths.extend(p for p in (ROOT/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    paths += list((ROOT/'PS2Recomp').glob('CMakeLists.txt'))
    return {p.relative_to(ROOT).as_posix():sha(p) for p in sorted(set(paths))}

def inventory(base, paths):
    files=[]
    for path in paths:
        files.extend([path] if path.is_file() else [p for p in path.rglob('*') if p.is_file()])
    return {p.relative_to(base).as_posix():sha(p) for p in sorted(set(files))}

def write(path,data):
    temp=path.with_name(path.name+'.'+uuid.uuid4().hex+'.tmp')
    temp.write_text(json.dumps(data,indent=2,sort_keys=True)+'\n')
    os.replace(temp,path)

def verify(manifest, generated_dir=None):
    data=json.loads(manifest.read_text())
    if data.get('schema')!=1 or data['dependencies']!=dependencies(): raise ValueError('Stale generation dependencies; rerun setup')
    if not data.get('outputs'): raise ValueError('Empty generation inventory')
    base=manifest.parent
    for rel,digest in data['outputs'].items():
        p=(base/rel).resolve()
        if base.resolve() not in p.parents or not p.is_file() or sha(p)!=digest: raise ValueError('Missing/changed generation: '+rel)
    generated=base/data['generated_dir']
    if generated_dir is not None and generated.resolve()!=Path(generated_dir).resolve(): raise ValueError('Manifest belongs to a different generated directory')
    actual=inventory(base,[generated])
    expected={k:v for k,v in data['outputs'].items() if k.startswith(data['generated_dir']+'/')}
    if actual!=expected: raise ValueError('Generated file set changed')
    return data

if __name__=='__main__':
    try: verify(Path(sys.argv[1]), sys.argv[2] if len(sys.argv)>2 else None)
    except (OSError,ValueError,KeyError) as e: sys.exit(str(e))
