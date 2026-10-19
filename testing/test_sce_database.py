"""Offline hash/cache regression tests; all Git calls are mocked."""
from pathlib import Path
import hashlib, os, sys, tempfile, unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"setup"))
import sce_database as db

class DatabaseTests(unittest.TestCase):
    def test_fetch_verify_reuse_and_reject_corrupt_cache(self):
        work=Path(tempfile.mkdtemp())
        payload=b"synthetic signature fixture, no SDK content"
        digest=hashlib.sha256(payload).hexdigest()
        with patch.object(db,"SHA256",digest), patch.object(db.subprocess,"run") as run, patch.object(db.subprocess,"check_output",return_value=payload) as show:
            target=db.ensure(work)
            self.assertEqual(target.read_bytes(),payload)
            self.assertEqual(run.call_count,2)
            fetch=run.call_args.args[0]
            self.assertEqual(fetch[-4:],["fetch","--depth=1",db.UPSTREAM,db.REVISION])
            self.assertEqual(show.call_args.args[0][-1],db.REVISION+":"+db.SOURCE_PATH)
            run.reset_mock();show.reset_mock()
            self.assertEqual(db.ensure(work),target)
            run.assert_not_called();show.assert_not_called()
            target.write_bytes(b"corrupt")
            with self.assertRaises(ValueError):db.ensure(work)
            run.assert_not_called();show.assert_not_called()

    def test_bad_fetch_never_publishes_and_retry_succeeds(self):
        work=Path(tempfile.mkdtemp());payload=b"synthetic fixture"
        digest=hashlib.sha256(payload).hexdigest()
        with patch.object(db,"SHA256",digest),patch.object(db.subprocess,"run"),patch.object(db.subprocess,"check_output",return_value=b"wrong") as show:
            with self.assertRaises(ValueError):db.ensure(work)
            self.assertFalse(list(work.glob("sce-database-*.h")))
            show.return_value=payload
            self.assertEqual(db.ensure(work).read_bytes(),payload)

    def test_interrupted_publication_can_retry(self):
        work=Path(tempfile.mkdtemp());payload=b"synthetic fixture"
        digest=hashlib.sha256(payload).hexdigest()
        with patch.object(db,"SHA256",digest),patch.object(db.subprocess,"run"),patch.object(db.subprocess,"check_output",return_value=payload):
            with patch.object(db.os,"replace",side_effect=OSError("interrupted")):
                with self.assertRaises(OSError):db.ensure(work)
            self.assertFalse(list(work.glob("sce-database-*.h")))
            self.assertEqual(len(list(work.glob("*.pending"))),1)
            self.assertEqual(db.ensure(work).read_bytes(),payload)

if __name__=="__main__":unittest.main()
