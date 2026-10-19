"""Regression tests for persistent cards and strict state/counter qualification."""
import csv,importlib.util,json,os,sys,tempfile,unittest,zipfile
from pathlib import Path
from unittest.mock import patch
ROOT=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(ROOT/'setup'),str(ROOT/'testing')]
import cards,check_gh,check_static_coverage

class ReleaseTests(unittest.TestCase):
 def test_card_migrates_once_and_survives_generations(self):
  work=Path(tempfile.mkdtemp());old=work/'generation-old';(old/'mc0'/'slot').mkdir(parents=True)
  (old/'mc0'/'slot'/'data').write_bytes(b'old card')
  root=cards.initialize(work,old/'SLUS_206.68')
  self.assertEqual((root/'slot'/'data').read_bytes(),b'old card')
  (root/'slot'/'data').write_bytes(b'new card')
  self.assertEqual(cards.initialize(work,old/'SLUS_206.68'),root)
  self.assertEqual((root/'slot'/'data').read_bytes(),b'new card')
  for i in range(7):
   (root/'slot'/'data').write_bytes(str(i).encode());cards.backup(root)
  backups=root.parent/'backups'
  self.assertEqual(len(list(backups.glob('*.zip'))),5)
  with zipfile.ZipFile(backups/'mc0-2.zip') as z:self.assertEqual(z.read('slot/data'),b'6')
 def test_gh_labels_diagnostic_state_and_counters_exact(self):
  off={'hashes':[['65536','abc']],'labels':[10],'counters':[1,2,3,4,5,6]}
  on=dict(off,labels=[12]);self.assertTrue(check_gh.compare(off,on))
  self.assertFalse(check_gh.compare(off,dict(on,hashes=[['65536','def']])))
  self.assertFalse(check_gh.compare(off,dict(on,counters=[1,2,3,4,5,7])))
  self.assertFalse(check_gh.compare(dict(off,hashes=[]),dict(on,hashes=[])))
 def test_gh_accepts_mirrored_verdict_and_rejects_conflicting_tuple(self):
  folder=Path(tempfile.mkdtemp())
  result='[script] RESULT PASS code=0 vs=1 pads=2 cyc=3 cd=4 i5=5 mscal=6\n'
  (folder/'stdout.log').write_text(result)
  (folder/'stderr.log').write_text('[vu1aot:hash] calls=65536 vs=12 hash=abc\n'+result)
  self.assertEqual(check_gh.read(folder)['counters'],[1,2,3,4,5,6])
  (folder/'stdout.log').write_text(result.replace('cyc=3','cyc=9'))
  with self.assertRaises(ValueError):check_gh.read(folder)
 def test_gh_ignores_only_post_verdict_hashes(self):
  folder=Path(tempfile.mkdtemp())
  verdict='[script] RESULT PASS code=0 vs=100 pads=2 cyc=3 cd=4 i5=5 mscal=6\n'
  first='[vu1aot:hash] calls=65536 vs=99 hash=abc\n'
  # Same presentation label deliberately proves this is a verdict boundary,
  # rather than a heuristic cutoff using diagnostic vsync labels.
  suffix='[vu1aot:hash] calls=131072 vs=99 hash=def\n'
  (folder/'stdout.log').write_text(verdict)
  (folder/'stderr.log').write_text(first+verdict+suffix)
  expected=check_gh.read(folder)
  self.assertEqual(expected['hashes'],[['65536','abc']])
  (folder/'stderr.log').write_text(first+suffix+verdict)
  self.assertFalse(check_gh.compare(expected,check_gh.read(folder)))

 def test_coverage_rejects_disabled_and_unbounded_interpreter(self):
  p=Path(tempfile.mkdtemp())/'metrics.csv';p.write_text('vu1_interp_calls,VU_interp_calls\n0,0\n')
  self.assertFalse(check_static_coverage.coverage('declined=0',p)['pass'])
  self.assertTrue(check_static_coverage.coverage('[vu1aot] progress accepted=1024 [perfscope:vu1] interp_calls=0',p)['pass'])
  p.write_text('vu1_interp_calls,VU_interp_calls\n0,65\n')
  self.assertFalse(check_static_coverage.coverage('[vu1aot] progress accepted=1024 [perfscope:vu1] interp_calls=0',p)['pass'])
if __name__=='__main__':unittest.main()
