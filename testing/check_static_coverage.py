import csv,re

def coverage(text,path):
 accepted=[int(x) for x in re.findall(r'\[vu1aot\].*?accepted=(\d+)',text)]
 rows=[]
 if path.exists():
  with path.open() as source:rows=list(csv.DictReader(source))
 # Periodic CSV rows contain window deltas; do not mix them with a cumulative total.
 columns=('vu1_interp_calls','VU_interp_calls')
 valid=bool(rows) and all(all(k in r and r[k].isdigit() for k in columns) for r in rows)
 interp=sum(sum(int(r[k]) for k in columns) for r in rows) if valid else None
 totals=[int(x) for x in re.findall(r'\[perfscope:vu1\] interp_calls=(\d+)',text)]
 exact=totals[-1] if totals else None
 return {'terminal_interpreter_calls':exact,'pass':bool(totals) and exact<=64 and bool(accepted) and max(accepted)>0 and valid and interp<=64,'accepted_last':accepted[-1] if accepted else None,'interpreter_calls_observed':interp,'interpreter_limit':64,'csv_rows':len(rows),'note':'Exact terminal EE+VU count plus periodic CSV sanity check; smoke must accept static calls'}

if __name__=='__main__':
 import json,sys
 from pathlib import Path
 folder=Path(sys.argv[1])
 text='\n'.join((folder/n).read_text(errors='replace') for n in ('stderr.log','stdout.log'))
 result=coverage(text,Path(sys.argv[2]))
 print(json.dumps(result));sys.exit(0 if result['pass'] else 1)
