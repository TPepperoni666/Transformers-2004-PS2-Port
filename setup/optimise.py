"""Optional private MSVC PGO. Profiles never enter the checkout or a package."""
from pathlib import Path
import ctypes,hashlib,json,os,re,shutil,subprocess,sys,time,uuid
from generation import write,sha
REFS={'level_mvp':[5396,2021,26522953417,2070,2422,116720], 'level_mvp_cine':[9604,4121,47206531601,3443,4526,981847]}
SETTINGS='PS2X_GS_BACKEND=PGS;PS2X_GS_THREAD=1;PS2X_STOREIMAGE_LAG=1;PS2X_LOAD_SKIP=4;PS2X_QUIET_IO=1;PS2X_REALTIME=0;PS2X_AUDIO=0;PS2X_AUDIO_STATS=0;PS2X_PGS_PIPECACHE=0;PS2X_VU1_THREAD=3;PS2X_KERNEL_HEAP=1;PS2X_PERFSCOPE=1;PS2X_AOTCOV4_METRICS=1;PS2X_VU1_AOT=1;PS2X_VU1_AOT_STATIC=1;PS2X_TEST_RTC_EPOCH=1791817200'
def _cache_values(gen):
    cache=gen/'build-game/CMakeCache.txt'
    if not cache.exists():return {}
    return {line.split('=',1)[0].split(':',1)[0]:line.split('=',1)[1]
            for line in cache.read_text().splitlines() if '=' in line and not line.startswith(('#','//'))}

def _verify_cached_compiler(values,vc,expected):
    compiler=values.get('CMAKE_CXX_COMPILER')
    if compiler:
        directory=Path(compiler.strip().strip('"')).parent
        for name in ('cl.exe','c1xx.dll','c2.dll'):
            path=directory/name
            if not path.is_file() or sha(path)!=expected[name]:
                raise RuntimeError('Ordinary build uses a different MSVC toolchain; run setup with a fresh external work folder')
    linker=values.get('CMAKE_LINKER')
    if linker:
        path=Path(linker.strip().strip('"'))
        if not path.is_file() or sha(path)!=expected['link.exe']:
            raise RuntimeError('Ordinary build uses a different linker; run setup with a fresh external work folder')

def _static_inputs(values,cmake_args):
    folder=next((arg.split('=',1)[1] for arg in reversed(cmake_args) if arg.startswith('-DPS2X_VU1_STATIC_DIR=')),values.get('PS2X_VU1_STATIC_DIR'))
    if not folder:return {}
    folder=Path(folder)
    if not (folder/'sources.cmake').is_file():raise RuntimeError('Missing FAST static source package; rerun setup')
    return {p.relative_to(folder).as_posix():sha(p) for p in sorted(folder.rglob('*'))
            if p.is_file() and '__pycache__' not in p.parts}

class Memory(ctypes.Structure):
    _fields_=[('length',ctypes.c_ulong),('load',ctypes.c_ulong)]+[(n,ctypes.c_ulonglong) for n in ('totalPhys','availPhys','totalPage','availPage','totalVirtual','availVirtual','extended')]
def memory_admission(deadline,record):
    while time.time()<deadline:
        m=Memory();m.length=ctypes.sizeof(m)
        if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):raise OSError('Cannot read available memory')
        record({'available_physical':m.availPhys,'minimum_physical':12*2**30})
        if m.availPhys>=12*2**30:return
        time.sleep(30)
    raise TimeoutError('PGO link memory admission expired')
def link_main():
    deadline=float(os.environ['PS2X_OPTIMISE_EXPIRY'])
    memory_admission(deadline,lambda r: print('PGO link admission',json.dumps(r),flush=True))
    raise SystemExit(subprocess.call(sys.argv[2:],stdout=sys.stdout,stderr=sys.stderr,creationflags=0x08004000))
def optimise(root,gen,game,iso,output,manifest,cmake_args):
    if os.name!='nt' or not os.environ.get('VCToolsInstallDir'):raise RuntimeError('--optimise needs an x64 VS developer command prompt on Windows')
    vc=Path(os.environ['VCToolsInstallDir'])/'bin/Hostx64/x64'
    for name in ('cl.exe','pgomgr.exe','pgosweep.exe','pgort140.dll','dumpbin.exe'):
        if not (vc/name).is_file():raise RuntimeError('Missing MSVC PGO tool: '+name)
    if not json.loads((game/'disc.json').read_text()).get('static_vu1'):raise RuntimeError('--optimise currently requires the FAST static VU1 build; see README')
    began=time.time();expiry=min(began+16*3600,float(os.environ.get('PS2X_OPTIMISE_EXPIRY',began+16*3600)))
    os.environ['PS2X_OPTIMISE_EXPIRY']=str(expiry)
    toolchain={n:sha(vc/n) for n in ('cl.exe','c1xx.dll','c2.dll','link.exe','pgomgr.exe','pgosweep.exe','pgort140.dll','dumpbin.exe')}
    cached=_cache_values(gen);_verify_cached_compiler(cached,vc,toolchain)
    fingerprint={'toolchain':toolchain,'generated_manifest':sha(manifest),'cmake_args':cmake_args,'static_package_sources':_static_inputs(cached,cmake_args),
      'coverage_gate':sha(root/'testing/check_static_coverage.py'),'runner':sha(root/'testing/run_test.ps1'),'preset':sha(root/'launch/play_env.txt'),
      'sources':{p.relative_to(root).as_posix():sha(p) for folder in ('setup','testing/pgo','PS2Recomp','deps/parallel-gs') for p in (root/folder).rglob('*') if p.is_file() and '__pycache__' not in p.parts}}
    key=hashlib.sha256(json.dumps(fingerprint,sort_keys=True).encode()).hexdigest()
    private=gen/'optimise';private.mkdir(exist_ok=True)
    statefile=private/'state.json';state=json.loads(statefile.read_text()) if statefile.exists() else {}
    if state.get('fingerprint')!=key:
        state={'fingerprint':key,'attempt':uuid.uuid4().hex,'steps':{},'started':began}
    work=private/state['attempt'];work.mkdir(exist_ok=True)
    write(work/'inputs.json',fingerprint)
    def checkpoint(phase,**kw):
        state.update(phase=phase,updated=time.time(),**kw);write(statefile,state)
    def command(label,args,timeout=3600,cwd=None,env=None):
        checkpoint(label)
        phase_started=time.time()
        log=work/(label+'-'+uuid.uuid4().hex+'.log')
        with log.open('w') as f:
            print('+',subprocess.list2cmdline(list(map(str,args))),flush=True)
            child=subprocess.Popen(list(map(str,args)),stdout=f,stderr=subprocess.STDOUT,cwd=cwd or work,env=env,creationflags=0x08004000)
            checkpoint(label,child_pid=child.pid,log=str(log))
            try:code=child.wait(timeout=max(1,min(timeout,expiry-time.time())))
            except subprocess.TimeoutExpired:
                subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],stdout=f,stderr=f,creationflags=0x08000000)
                raise TimeoutError(label+' timed out; ordinary build remains available')
        state.setdefault('events',[]).append({'phase':label,'seconds':time.time()-phase_started,'exit_code':code,'log':str(log)})
        checkpoint(label)
        if code:raise RuntimeError(label+' failed; see '+str(log))
        return log.read_text(errors='replace')
    fallback=game/'fallback';fallback.mkdir(exist_ok=True)
    baseline=fallback/'ps2EntryRunner.exe'
    if not baseline.exists() or sha(baseline)!=sha(game/'ps2EntryRunner.exe'):
        shutil.copy2(game/'ps2EntryRunner.exe',baseline)
    checkpoint('preparing',fallback=str(baseline))
    build=gen/'build-game';db=work/'local.pgd';original=work/'unmerged.pgd'
    launcher=Path(sys.executable).as_posix()+';'+(root/'setup/optimise.py').as_posix()+';--link'
    # Preserve ordinary native EE objects/PCH; only engine targets gain /GL.
    # Both profile links use the same engine objects and this private PGD.
    common=['cmake','-S',root/'PS2Recomp','-B',build,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DPS2X_BUILD_RECOMP=OFF','-DPS2X_BUILD_ANALYZER=OFF','-DPS2X_BUILD_TEST=OFF','-DPS2X_BUILD_STUDIO=OFF','-DPS2X_BUILD_RUNNER=ON',f'-DPS2X_GENERATED_DIR={output}',f'-DPS2X_GENERATION_MANIFEST={manifest}','-DPS2X_WITH_VU1_AOT=OFF','-DPS2X_WITH_PGS=ON','-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF','-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG','-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG',*cmake_args,f'-DCMAKE_CXX_LINKER_LAUNCHER={launcher}',f'-DPS2X_PGO_DATABASE={db}','-DPS2X_PGO_GENERATED_EE=OFF']
    def link(phase):
        command('configure-'+phase,[*common,'-DPS2X_PGO_PHASE='+phase])
        command('build-'+phase,['cmake','--build',build,'--target','ps2EntryRunner','--parallel','4'],6*3600)
        paths=list(build.rglob('ps2EntryRunner.exe'))
        if len(paths)!=1:raise RuntimeError('Ambiguous PGO executable')
        return paths[0]
    instrumented=work/'instrumented';instrumented.mkdir(exist_ok=True)
    pgi=instrumented/'ps2EntryRunner.exe'
    if not state['steps'].get('instrumented') or not pgi.exists() or sha(pgi)!=state['steps']['instrumented']:
        exe=link('GENERATE')
        shutil.copy2(exe,pgi)
        for dll in game.glob('*.dll'):shutil.copy2(dll,instrumented/dll.name)
        shutil.copy2(vc/'pgort140.dll',instrumented/'pgort140.dll')
        shutil.copy2(game/'vu1known.bin',instrumented/'vu1known.bin')
        shutil.copy2(db,original)
        state['steps']['instrumented']=sha(pgi);state['steps']['unmerged']=sha(original);checkpoint('instrumented')
    if sha(original)!=state['steps']['unmerged']:raise RuntimeError('Unmerged PGO database changed')
    def route_run(label,exe,route,sweep=None):
        runwork=work/'runs'/(label+'-'+uuid.uuid4().hex)
        env=dict(os.environ)
        # Ambient development switches cannot alter a training route.
        for k in list(env):
            if k.startswith('PS2X_'):del env[k]
        csv=runwork/'coverage.csv'
        args=['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',root/'testing/run_test.ps1','-Exe',exe,'-Elf',gen/'SLUS_206.68','-Iso',iso,'-Script',route,'-Work',runwork,'-Seconds','3000','-Env',SETTINGS+f';PS2X_PERFSCOPE_CSV={csv}']
        if sweep:
            args+=['-PgoSweepTool',vc/'pgosweep.exe','-PgoSweepOutput',sweep]
            env['VCPROFILE_PATH']=str(work/'automatic-profiles'/runwork.name);Path(env['VCPROFILE_PATH']).mkdir(parents=True)
        command(label,args,3200,env=env)
        text='\n'.join((runwork/n).read_text(errors='replace') for n in ('stdout.log','stderr.log'))
        matches=re.findall(r'\[script\] RESULT PASS .*?vs=(\d+) pads=(\d+) cyc=(\d+) cd=(\d+) i5=(\d+) mscal=(\d+)',text)
        if not matches:raise RuntimeError(label+' no PASS tuple')
        row={'counters':list(map(int,matches[-1])),'work':str(runwork)}
        if sweep:
            if 'PGOSWEEP_EXIT=0' not in (runwork/'env.txt').read_text() or not sweep.is_file() or not sweep.stat().st_size:raise RuntimeError('Explicit collection missing')
            row['profile']=str(sweep);row['sha256']=sha(sweep)
        return row
    try:
        training=state['steps'].setdefault('training',{})
        for route in sorted((root/'testing/pgo').glob('*.tfs')):
            previous=training.get(route.stem)
            if previous and Path(previous['profile']).exists() and sha(Path(previous['profile']))==previous['sha256']:continue
            ref1=route_run('reference1-'+route.stem,baseline,route)
            ref2=route_run('reference2-'+route.stem,baseline,route)
            if ref1['counters']!=ref2['counters']:raise RuntimeError('Fresh-card route not repeatable: '+route.stem)
            profile=work/(route.stem+'-'+uuid.uuid4().hex+'.pgc')
            observed=route_run('train-'+route.stem,pgi,route,profile)
            if observed['counters']!=ref1['counters']:raise RuntimeError('Instrumented route counters differ: '+route.stem)
            validation=work/('validate-'+uuid.uuid4().hex+'.pgd');shutil.copy2(original,validation)
            collected=work/('validate!'+uuid.uuid4().hex+'.pgc');shutil.copy2(profile,collected)
            command('validate-profile-'+route.stem,[vc/'pgomgr.exe','/merge:1',collected,validation])
            summary=command('validate-summary-'+route.stem,[vc/'pgomgr.exe','/summary','/unique',validation])
            dynamic=re.search(r'Dynamic instructions:\s*([\d,]+)',summary)
            if not dynamic or int(dynamic[1].replace(',',''))==0:raise RuntimeError('Empty training profile: '+route.stem)
            observed['dynamic_instructions']=int(dynamic[1].replace(',',''))
            observed['references']=[ref1,ref2];training[route.stem]=observed;checkpoint('training')
        if len(training)!=len(list((root/'testing/pgo').glob('*.tfs'))):raise RuntimeError('Incomplete training set')
        # Always rebuild the merge from the immutable instrumented database; no double merge on resume.
        shutil.copy2(original,db)
        for name,row in sorted(training.items()):
            pgc=work/('local!'+name+'.pgc');shutil.copy2(row['profile'],pgc)
            command('merge-'+name,[vc/'pgomgr.exe','/merge:1',pgc,db])
        summary=command('profile-summary',[vc/'pgomgr.exe','/summary','/unique',db])
        count=re.search(r'Dynamic instructions:\s*([\d,]+)',summary)
        if not count or int(count[1].replace(',',''))==0:raise RuntimeError('Empty merged profile')
        write(work/'profile-summary.json',{'dynamic_instructions':int(count[1].replace(',','')),'text':summary})
        exe=link('USE')
        imports=command('verify-imports',[vc/'dumpbin.exe','/imports',exe])
        if re.search(r'(?im)^\s*pgort\d+\.dll\s*$',imports):
            raise RuntimeError('Optimised executable still imports the PGO collector runtime')
        gates={}
        for name,expected in REFS.items():
            route=work/(name+'.tfs')
            route.write_text((root/'testing/mvp'/(name+'.tfs')).read_text().replace('dumpsteps on','dumpsteps off'))
            row=route_run('verify-'+name,exe,route)
            if row['counters']!=expected:raise RuntimeError('Optimised counter mismatch: '+name)
            gates[name]=row
        smoke=route_run('boot-smoke',exe,root/'testing/pgo/new-game-cinematics.tfs')
        if smoke['counters']!=REFS['level_mvp_cine']:raise RuntimeError('Optimised smoke mismatch')
        sys.path.insert(0,str(root/'testing'))
        from check_static_coverage import coverage
        smokework=Path(smoke['work'])
        text='\n'.join((smokework/n).read_text(errors='replace') for n in ('stdout.log','stderr.log'))
        smoke['static_coverage']=coverage(text,smokework/'coverage.csv')
        if not smoke['static_coverage']['pass']:raise RuntimeError('Optimised smoke did not use static VU1 or exceeded interpreter bound')
        state['steps']['gates']=gates;state['steps']['smoke']=smoke
        pending=game/('optimised-'+uuid.uuid4().hex+'.pending');shutil.copy2(exe,pending)
        os.replace(pending,game/'ps2EntryRunner.exe')
        checkpoint('complete',seconds=time.time()-state['started'],optimised_sha256=sha(game/'ps2EntryRunner.exe'),private_bytes=sum(p.stat().st_size for p in private.rglob('*') if p.is_file()))
        print('Local optimisation complete. Ordinary executable:',baseline,flush=True)
    except Exception as e:
        checkpoint('failed',error=str(e));raise
if __name__=='__main__' and '--link' in sys.argv:link_main()
