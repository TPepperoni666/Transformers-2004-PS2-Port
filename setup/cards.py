"""Persistent user card and five fixed backup slots (stdlib only)."""
import argparse, json, os, shutil, zipfile
from pathlib import Path

def initialize(work, old_elf=None):
    root=Path(work)/'saves'/'mc0'
    if not root.exists():
        old=Path(old_elf).parent/'mc0' if old_elf else None
        root.parent.mkdir(parents=True,exist_ok=True)
        if old and old.is_dir(): shutil.copytree(old,root)
        else: root.mkdir()
    return root

def backup(root):
    root=Path(root)
    if not root.is_dir(): raise ValueError('Memory card directory is missing: '+str(root))
    backups=root.parent/'backups';backups.mkdir(exist_ok=True)
    state=backups/'rotation.json'
    slot=(json.loads(state.read_text())['last_slot']+1)%5 if state.exists() else 0
    with zipfile.ZipFile(backups/f'mc0-{slot+1}.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for p in sorted(root.rglob('*')):
            if p.is_file(): archive.write(p,p.relative_to(root).as_posix())
    state.write_text(json.dumps({'last_slot':slot})+'\n')

if __name__=='__main__':
    a=argparse.ArgumentParser();a.add_argument('--disc',type=Path,required=True);args=a.parse_args()
    if not os.environ.get('PS2X_SCRIPT'):
        backup(json.loads(args.disc.read_text())['mc_root'])
