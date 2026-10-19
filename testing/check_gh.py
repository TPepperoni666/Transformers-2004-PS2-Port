"""Controlled GH: ordered call/hash sequence plus exact terminal counters."""
import json, re, sys
from pathlib import Path

def read(folder):
    streams=[(Path(folder)/n).read_text(errors='replace') for n in ('stderr.log','stdout.log')]
    text='\n'.join(streams)
    # The EE may continue during the terminal screenshot and shutdown. Hash only
    # work before each stream's first verdict; retain every pre-verdict checkpoint.
    bounded='\n'.join(re.split(r'^\[script\] RESULT ',stream,maxsplit=1,flags=re.M)[0] for stream in streams)
    checkpoints=re.findall(r'^\[vu1aot:hash\] calls=(\d+) vs=(\d+) hash=([0-9a-f]+)$',bounded,re.M)
    counters=re.findall(r'\[script\] RESULT PASS .*?vs=(\d+) pads=(\d+) cyc=(\d+) cd=(\d+) i5=(\d+) mscal=(\d+)',text)
    counters=list(dict.fromkeys(counters))
    if not checkpoints or len(counters)!=1: raise ValueError('Missing GH checkpoints or unique terminal PASS tuple')
    return {'hashes':[[c,h] for c,v,h in checkpoints],'labels':[int(v) for c,v,h in checkpoints],'counters':list(map(int,counters[0]))}

def compare(off,on):
    return bool(off['hashes']) and off['hashes']==on['hashes'] and off['counters']==on['counters']

if __name__=='__main__':
    off,on=map(read,sys.argv[1:3]);ok=compare(off,on)
    print(json.dumps({'pass':ok,'checkpoints':[len(off['hashes']),len(on['hashes'])],'vsync_labels':[off['labels'],on['labels']]}))
    sys.exit(0 if ok else 1)
