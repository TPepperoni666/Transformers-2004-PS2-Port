"""Fail-closed source-distribution check; run before copying/publishing the stage.

Hashes supplement path/content checks. Passing is not a legal provenance audit.
"""
from pathlib import Path
import argparse, hashlib, json, re, sys

ALLOWED = {'.c','.cc','.cpp','.h','.hh','.hpp','.inl','.cmake','.txt','.md','.py','.ps1','.sh','.bat','.cmd','.json','.toml','.glsl','.vert','.frag','.comp','.inc','.def','.natvis','.yml','.yaml','.tfs','.gitignore','.gitmodules','.rst','.css','.html','.xml','.map','.conf','.patch','.diff','.cfg','.template','.list','.jinja','.j2','.m','.mm','.s','.S','.geom','.tesc','.tese','.license','.idl','.rc','.manifest','.awk','.bzl','.bazel','.gn','.gni','.td','.adoc','.rules','.pc','.xslt','.xsl','.hpp.in','.h.in'}
def reviewed_hash(table,path,digest):
    value=table.get(path,[])
    return digest in (value if isinstance(value,list) else [value])

def scan(root, policy_root=None):
    errors=[]; files=0; size=0
    if not root.is_dir():
        print(f'Not a source directory: {root}',file=sys.stderr)
        return 1
    policy_root = policy_root or Path(__file__).parent
    hashes_file=policy_root/'private_hashes.json'
    if not hashes_file.is_file():
        print('Missing private-package hash inventory',file=sys.stderr)
        return 1
    hashes=set(json.loads(hashes_file.read_text())['sha256'])
    shared_file=policy_root/'reviewed_shared_sources.json'
    shared=json.loads(shared_file.read_text()) if shared_file.exists() else {}
    reviewed_file=policy_root/'reviewed_large_sources.json'
    reviewed=json.loads(reviewed_file.read_text()) if reviewed_file.exists() else {}
    image_file=policy_root/'reviewed_images.json'
    approved_images=json.loads(image_file.read_text()) if image_file.exists() else {}
    for p in sorted(root.rglob('*')):
        rel=p.relative_to(root)
        if '.git' in rel.parts: continue
        if p.is_symlink(): errors.append(f'{rel}: symlink requires review'); continue
        if not p.is_file(): continue
        files+=1; n=p.stat().st_size; size+=n
        low=str(rel).replace('\\','/').lower(); name=p.name.lower()
        if p.suffix.lower() in {'.map','.pdb'}: errors.append(f'{rel}: build/debug product')
        if any(x.lower().startswith(('vu1_aot_private','vu1_static_private','pkg','output_','results_','vu1dump','capture','mc_seed','mc_playtest','build')) for x in rel.parts[:-1]) or 'ps2xruntime/src/runner/' in low:
            errors.append(f'{rel}: private/generated/build path')
        if re.search(r'_0x[0-9a-f]+\.(cpp|h)$',name) or name in {'slus_206.68','ps2_recompiled_functions.h','ps2_recompiled_stubs.h','register_functions.cpp','vu1known.bin'}:
            errors.append(f'{rel}: generated/game filename')
        # Only inventoried approved comparison images are accepted.
        screenshot = (len(rel.parts) == 3 and rel.parts[:2] == ('docs', 'images')
                      and p.suffix == '.png' and rel.as_posix() in approved_images)
        special=name.startswith(('license','copying','notice','authors','changelog','readme','cmakelists','makefile')) or name in {'.gitignore','.gitmodules','.clang-format','.editorconfig'} or rel.as_posix() == 'tools/git-hooks/pre-commit'
        if p.suffix not in ALLOWED and not special and not screenshot: errors.append(f'{rel}: unreviewed file type {p.suffix}')
        data=p.read_bytes()
        if n>8*1024*1024 and not reviewed_hash(reviewed,rel.as_posix(),hashlib.sha256(data).hexdigest()): errors.append(f'{rel}: oversized unreviewed source ({n} bytes)')
        if screenshot and not data.startswith(b'\x89PNG\r\n\x1a\n'):
            errors.append(f'{rel}: screenshot exception requires PNG signature')
        digest=hashlib.sha256(data).hexdigest()
        if screenshot and approved_images[rel.as_posix()]!=digest: errors.append(f'{rel}: changed comparison image requires review')
        if digest in hashes and not reviewed_hash(shared,rel.as_posix(),digest): errors.append(f'{rel}: private package hash')
        if not screenshot and (b'\x00' in data or data.startswith((b'\x7fELF',b'MZ',b'PK\x03\x04',b'\x89PNG',b'\xff\xd8\xff'))): errors.append(f'{rel}: binary magic/content')
        if p.suffix in {'.cpp','.h','.hpp'} and re.search(rb'void\s+(?:sub|fn|ctor)_[0-9a-fA-F]{6,8}\s*\(\s*(?:PS2Runtime|CpuContext|R5900Context)',data):
            errors.append(f'{rel}: generated function definition')
    if not files: errors.append('No source files found')
    print(f'{files} files; {size} bytes; {len(errors)} violations; {len(hashes)} private hashes')
    for e in errors: print(e,file=sys.stderr)
    return int(bool(errors))
if __name__=='__main__':
    a=argparse.ArgumentParser(); a.add_argument('root',nargs='?',type=Path,default=Path(__file__).resolve().parents[1]); args=a.parse_args()
    sys.exit(scan(args.root.resolve()))
