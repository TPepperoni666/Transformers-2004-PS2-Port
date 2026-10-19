"""Inspect index blobs, never unstaged working-tree content."""
import subprocess, tempfile, sys
from pathlib import Path
from check_no_game_data import scan

def git(*args):
    return subprocess.check_output(['git', *args])

def main():
    root=Path(git('rev-parse','--show-toplevel').decode().strip())
    changed_paths=set(git('diff','--cached','--name-only','-z').split(b'\0'))
    entries=git('ls-files','--stage','-z').split(b'\0')
    errors=[]
    # A private snapshot is retained: no workspace files are removed.
    snapshot=Path(tempfile.mkdtemp(prefix='ps2-index-'))
    blob_process=subprocess.Popen(['git','cat-file','--batch'],stdin=subprocess.PIPE,stdout=subprocess.PIPE)
    for entry in entries:
        if not entry: continue
        header, raw=entry.split(b'\t',1); mode,oid,stage=header.split()
        rel=raw.decode('utf-8',errors='surrogateescape')
        if stage!=b'0' or mode not in (b'100644',b'100755'):
            errors.append(rel+': unresolved, symlink or non-file index entry'); continue
        blob_process.stdin.write(oid+b'\n');blob_process.stdin.flush()
        response=blob_process.stdout.readline().split()
        if len(response)!=3 or response[1]!=b'blob': raise RuntimeError('Index blob unavailable')
        data=blob_process.stdout.read(int(response[2]));blob_process.stdout.read(1)
        # Legacy vendored files over this limit remain auditable by the source
        # scanner; all staged additions/modifications are subject to the hook cap.
        if raw in changed_paths and len(data)>1024*1024: errors.append(rel+': staged file exceeds 1 MiB')
        target=snapshot/rel; target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(data)
    blob_process.stdin.close();blob_process.wait()
    policy=snapshot/'tools'
    for name in ('private_hashes.json','reviewed_shared_sources.json','reviewed_large_sources.json','reviewed_images.json'):
        if not (policy/name).is_file(): errors.append('Missing staged policy inventory: '+name)
    try: result=scan(snapshot,policy)
    except (ValueError,KeyError,OSError) as e: errors.append('Invalid staged policy: '+str(e)); result=1
    for error in errors: print(error,file=sys.stderr)
    return int(bool(errors) or result)
if __name__=='__main__': sys.exit(main())
