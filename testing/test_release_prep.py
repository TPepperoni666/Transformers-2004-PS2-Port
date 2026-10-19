"""Offline release-prep gates; no game, compiler or disc needed."""
import importlib.util, json, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'setup'),str(ROOT/'tools')]
import generation
from check_generation_equal import compare

def load():
    spec=importlib.util.spec_from_file_location('byod',ROOT/'setup/setup.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

class GenerationTests(unittest.TestCase):
    def test_inventory_rejects_mutation_extra_and_stale(self):
        base=Path(tempfile.mkdtemp());out=base/'output';out.mkdir();p=out/'a.cpp';p.write_text('int a;')
        manifest=base/'generation.json'
        generation.write(manifest,{'schema':1,'dependencies':generation.dependencies(),'generated_dir':'output','outputs':generation.inventory(base,[out])})
        generation.verify(manifest,out)
        with self.assertRaises(ValueError): generation.verify(manifest,base/'other')
        p.write_text('int b;')
        with self.assertRaises(ValueError): generation.verify(manifest)
        p.write_text('int a;');(out/'extra.cpp').write_text('extra')
        with self.assertRaises(ValueError): generation.verify(manifest)
        with patch.object(generation,'dependencies',return_value={}):
            with self.assertRaises(ValueError): generation.verify(manifest)

    def test_interruption_then_resume(self):
        m=load();base=Path(tempfile.mkdtemp());iso=base/'disc.iso';iso.write_bytes(b'iso');work=base/'work'
        calls=[];fail=[True]
        def run(*args,cwd=None):
            text=list(map(str,args));calls.append(text)
            if 'extract_elf.py' in text[1]: Path(text[-1]).write_bytes(b'elf')
            elif 'extract_runtime_files.py' in text[1]:
                dest=Path(text[-1]);(dest/'IOP').mkdir(exist_ok=True)
                (dest/'isomap.tsv').write_text('1\t1\t/TEST\n')
                (dest/'runtime-files.json').write_text('{}')
            elif 'index.py' in text[1]:
                dest=Path(text[-1]);dest.mkdir();(dest/'vu1known.bin').write_bytes(b'table')
            elif text[0]=='analyzer': Path(text[-1]).write_text('config')
            elif text[0]=='recomp':
                if fail[0]: fail[0]=False;raise RuntimeError('interrupted')
                dest=Path(cwd)/('mapped-output' if text[-1].endswith('mapped.toml') else 'output');dest.mkdir();(dest/'a.cpp').write_text('int a;')
            elif 'make_map.py' in text[1]: Path(text[text.index('--out-config')+1]).write_text('mapped')
        def executable(build,name):
            if name=='ps2EntryRunner':
                dest=build/'runner.exe';dest.parent.mkdir(exist_ok=True);dest.write_bytes(b'host');return dest
            return 'analyzer' if name=='ps2_analyzer' else 'recomp'
        def sha(path):
            if Path(path).name=='SLUS_206.68': return 'ab88780324b9492eca59016923d200dd1d95785fd725dd85f233c071805b4acb'
            return generation.sha(path)
        # Preserve the ELF header check while avoiding copyrighted fixture bytes.
        original_run=run
        def fixture_run(*args,**kw):
            original_run(*args,**kw)
            if len(args)>1 and 'extract_elf.py' in str(args[1]): Path(args[-1]).write_bytes(b'\x7fELFfixture')
        with patch.object(m,'ensure_sce_database',return_value=work/'synthetic-sdk.h'),patch.object(m,'run',fixture_run),patch.object(m,'executable',executable),patch.object(m,'sha',sha),patch.object(sys,'argv',['setup','--iso',str(iso),'--work',str(work)]):
            with self.assertRaises(RuntimeError): m.main()
            m.main();previous=len([c for c in calls if c[0]=='recomp']);m.main()
            self.assertEqual(previous,len([c for c in calls if c[0]=='recomp']))
            self.assertEqual(1,len([c for c in calls if len(c)>1 and 'extract_elf.py' in c[1]]))

    def test_comment_check_detects_changed_code(self):
        base=Path(tempfile.mkdtemp());a=base/'a';b=base/'b';a.mkdir();b.mkdir()
        # Python's own compiler verifies this synthetic generator comment edit.
        for source,dest in [('print("int a;")',a),('# comment-only change\nprint("int a;")',b)]:
            (dest/'a.cpp').write_bytes(subprocess.check_output([sys.executable,'-B','-c',source]))
        compare(a,b);(b/'a.cpp').write_text('int b;')
        with self.assertRaises(ValueError): compare(a,b)

class IndexTests(unittest.TestCase):
    def test_staged_content_and_limits(self):
        base=Path(tempfile.mkdtemp())
        def git(*args): return subprocess.check_output(['git','-C',str(base),*args],stderr=subprocess.DEVNULL)
        git('init');git('config','user.email','fixture@example.invalid');git('config','user.name','fixture')
        for rel in ('tools/check_no_game_data.py','tools/check_staged.py','tools/private_hashes.json','tools/reviewed_shared_sources.json','tools/reviewed_large_sources.json','tools/reviewed_images.json'):
            p=base/rel;p.parent.mkdir(exist_ok=True);p.write_bytes((ROOT/rel).read_bytes())
        p=base/'source.py';p.write_text('print(1)');git('add','.');git('commit','-m','fixture')
        def guard(): return subprocess.run([sys.executable,'-B',str(base/'tools/check_staged.py')],cwd=base,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL).returncode
        self.assertEqual(0,guard())
        p.write_bytes(b'MZ\x00private');git('add','source.py');p.write_text('safe worktree')
        self.assertNotEqual(0,guard())
        git('add','source.py');p.write_bytes(b'MZ\x00unstaged');self.assertEqual(0,guard())
        p.write_bytes(b'x'*(1024*1024+1));git('add','source.py');self.assertNotEqual(0,guard())
        p.write_text('safe');git('add','source.py')
        for name in ('renamed.iso','card.ps2','vu1known.bin','sub_0x001234.cpp'):
            (base/name).write_text('renamed private fixture');git('add',name)
            self.assertNotEqual(0,guard());git('reset','--',name)
        git('update-index','--force-remove','tools/private_hashes.json')
        self.assertNotEqual(0,guard());git('add','tools/private_hashes.json')
        oid=git('hash-object','source.py').decode().strip()
        git('update-index','--cacheinfo','120000,'+oid+',source.py')
        self.assertNotEqual(0,guard());git('add','source.py')
        runner=base/'PS2Recomp/ps2xRuntime/src/runner/renamed.cpp';runner.parent.mkdir(parents=True);runner.write_text('int a;');git('add',str(runner));self.assertNotEqual(0,guard())

if __name__=='__main__': unittest.main()
