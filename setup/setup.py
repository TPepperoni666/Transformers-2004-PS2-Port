"""Bring-your-own-disc orchestration. All derived products stay outside sources."""
from pathlib import Path
import argparse, os, subprocess, sys, shutil, json, uuid, time
from cards import initialize
from sce_database import ensure as ensure_sce_database
from generation import dependencies, inventory, write, verify, sha

ROOT=Path(__file__).resolve().parents[1]
def run(*args,cwd=None):
    print('+', ' '.join(map(str,args)),flush=True)
    child=subprocess.Popen(list(map(str,args)),cwd=cwd,stdout=sys.stdout,stderr=sys.stderr,creationflags=0x08004000 if os.name=='nt' else 0)
    limit=6*3600 if '--build' in args else 3600
    remaining=float(os.environ.get('PS2X_OPTIMISE_EXPIRY',time.time()+limit))-time.time()
    try: code=child.wait(timeout=max(1,min(limit,remaining)))
    except subprocess.TimeoutExpired:
        if os.name=='nt': subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],creationflags=0x08000000)
        else: child.kill()
        child.wait(); raise
    if code: raise subprocess.CalledProcessError(code,args)
def executable(build,name):
    candidates=list(build.rglob(name+('.exe' if os.name=='nt' else '')))
    if len(candidates)!=1: raise RuntimeError(f'Expected one {name}, found {candidates}')
    return candidates[0]
def main():
    a=argparse.ArgumentParser();a.add_argument('--iso',type=Path,required=True);a.add_argument('--work',type=Path,required=True);a.add_argument('--cmake-arg',action='append',default=[]);a.add_argument('--optimise',action='store_true',help='Optional local MSVC PGO training; keeps the ordinary build as fallback');args=a.parse_args()
    if args.optimise:
        if os.name!='nt' or not os.environ.get('VCToolsInstallDir'):
            raise SystemExit('--optimise requires an x64 VS developer shell on Windows')
        os.environ.setdefault('PS2X_OPTIMISE_EXPIRY',str(time.time()+16*3600))
    work=args.work.resolve();iso=args.iso.resolve()
    if work==ROOT or ROOT in work.parents: raise SystemExit('Private work must be outside the source tree')
    if not iso.is_file(): raise SystemExit('ISO does not exist')
    work.mkdir(parents=True,exist_ok=True)
    sce_database=ensure_sce_database(work)
    fingerprint={'dependencies':dependencies(),'iso_sha256':sha(iso),'cmake_args':args.cmake_arg}
    statefile=work/'setup-state.json'
    state=json.loads(statefile.read_text()) if statefile.exists() else {}
    old_elf=(work/('generation-'+state['generation'])/'SLUS_206.68') if state.get('generation') else None
    mc_root=initialize(work,old_elf)
    if state.get('fingerprint')!=fingerprint:
        state={'fingerprint':fingerprint,'generation':uuid.uuid4().hex,'steps':{}}
        write(statefile,state)
    work=work/('generation-'+state['generation']);work.mkdir(exist_ok=True)
    elf=work/'SLUS_206.68'
    if not elf.exists():
        pending=work/('extract-'+uuid.uuid4().hex+'.pending')
        run(sys.executable,ROOT/'setup/extract_elf.py','--iso',iso,'--out',pending)
        os.replace(pending,elf)
    ELF_SHA256="ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb"
    if sha(elf)!=ELF_SHA256: raise SystemExit('Changed extracted ELF; use a new --work directory')
    if elf.read_bytes()[:4]!=b'\x7fELF':raise SystemExit('SLUS_206.68 is not an ELF')
    # IOP module loading currently resolves host files beside the ELF.
    run(sys.executable,ROOT/'setup/extract_runtime_files.py','--iso',iso,'--work',work)
    index_record=state['steps'].get('index')
    if not index_record or inventory(work,[work/state.get('index','vu1-index')])!=index_record:
        index=work/('vu1-index-'+uuid.uuid4().hex)
        run(sys.executable,ROOT/'tools/vu1/index.py','--elf',elf,'--work',index)
        state['index']=index.name
        state['steps']['index']=inventory(work,[index]);write(statefile,state)
    index=work/state['index']
    effective_cmake=list(args.cmake_arg)
    if args.optimise and not any(x.startswith('-DPS2X_WITH_VU1_STATIC=') for x in effective_cmake):
        package=work/'local-static-package'
        if not (package/'package_manifest.json').exists():
            run(sys.executable,ROOT/'tools/vu1/package.py',elf,'--work',work/'local-static-work','--out',package)
        effective_cmake += ['-DPS2X_WITH_VU1_STATIC=ON',f'-DPS2X_VU1_STATIC_DIR={package.as_posix()}']
    tools=work/'build-tools';game=work/'build-game'
    compiler_args=['-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF']
    if os.name=='nt':
        compiler_args += ['-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG',
                          '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG',
                          '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO']
    run('cmake','-S',ROOT/'PS2Recomp','-B',tools,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DPS2X_BUILD_RUNTIME=OFF','-DPS2X_BUILD_TEST=OFF','-DPS2X_BUILD_STUDIO=OFF',*compiler_args,*effective_cmake,f'-DPS2X_SCE_DATABASE_HEADER={sce_database.as_posix()}')
    run('cmake','--build',tools,'--target','ps2_analyzer','ps2_recomp','--parallel','4')
    manifest=work/'generation.json'
    try:
        meta=verify(manifest)
        output=work/meta['generated_dir'];print('Resume: code generation')
    except (OSError,ValueError,KeyError):
        attempt=work/('code-'+uuid.uuid4().hex);attempt.mkdir()
        config=attempt/'game.toml'
        run(executable(tools,'ps2_analyzer'),elf,config,cwd=attempt)
        run(executable(tools,'ps2_recomp'),config,cwd=attempt)
        baseline=attempt/'output'
        output=attempt/'mapped-output'
        mapped=attempt/'mapped.toml'
        run(sys.executable,ROOT/'tools/make_map.py','--elf',elf,'--baseline',baseline,'--config',config,'--out-map',attempt/'functions.csv','--out-config',mapped,'--output',output,cwd=attempt)
        run(executable(tools,'ps2_recomp'),mapped,cwd=attempt)
        write(manifest,{'schema':1,'dependencies':dependencies(),'iso_sha256':fingerprint['iso_sha256'],'generated_dir':output.relative_to(work).as_posix(),'outputs':inventory(work,[elf,work/'IOP',work/'isomap.tsv',work/'runtime-files.json',index,attempt])})
    run('cmake','-S',ROOT/'PS2Recomp','-B',game,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DPS2X_BUILD_RECOMP=OFF','-DPS2X_BUILD_ANALYZER=OFF','-DPS2X_BUILD_TEST=OFF','-DPS2X_BUILD_STUDIO=OFF','-DPS2X_BUILD_RUNNER=ON',f'-DPS2X_GENERATED_DIR={output}',f'-DPS2X_GENERATION_MANIFEST={manifest}','-DPS2X_WITH_VU1_AOT=OFF','-DPS2X_WITH_PGS=ON',*compiler_args,*effective_cmake,'-DPS2X_PGO_PHASE=OFF')
    run('cmake','--build',game,'--target','ps2EntryRunner','--parallel','4')
    folder=work/'game';folder.mkdir(exist_ok=True)
    exe=executable(game,'ps2EntryRunner');shutil.copy2(exe,folder/exe.name)
    for dll in exe.parent.glob('*.dll'):shutil.copy2(dll,folder/dll.name)
    shutil.copy2(index/'vu1known.bin',folder/'vu1known.bin')
    cache=game/'CMakeCache.txt'
    static_vu1=cache.exists() and any(line in ('PS2X_WITH_VU1_STATIC:BOOL=ON','PS2X_WITH_VU1_STATIC:BOOL=TRUE','PS2X_WITH_VU1_STATIC:BOOL=1') for line in cache.read_text().splitlines())
    (folder/'disc.json').write_text(__import__('json').dumps({'iso':str(iso),'elf':str(elf),'mc_root':str(mc_root),'static_vu1':static_vu1},indent=2))
    if args.optimise:
        from optimise import optimise
        optimise(ROOT, work, folder, iso, output, manifest, effective_cmake)
    print('Built experimental game folder:',folder,'; fresh-disc route qualification still required. See docs/SETUP.md')
if __name__=='__main__':main()
