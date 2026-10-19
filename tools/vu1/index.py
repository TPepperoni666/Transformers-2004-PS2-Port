"""Extract verified ELF ranges privately and build the new-code detector table."""
import argparse,hashlib,importlib.util
from pathlib import Path
from package import ELF_SHA256,extract,R
def main():
 a=argparse.ArgumentParser();a.add_argument('--elf',type=Path,required=True);a.add_argument('--work',type=Path,required=True);args=a.parse_args()
 data=args.elf.read_bytes()
 if hashlib.sha256(data).hexdigest()!=ELF_SHA256:raise SystemExit('Refusing unsupported ELF SHA256')
 work=args.work.resolve()
 if work==R or R in work.parents:raise SystemExit('Work must be outside the source checkout')
 if work.exists() and any(work.iterdir()):raise SystemExit('Use a fresh empty work directory')
 manifest=extract(data,work/'extracted')
 spec=importlib.util.spec_from_file_location('known',R/'PS2Recomp/tools/gen_vu1known.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
 m.generate(args.elf,manifest,work/'vu1known.bin')
if __name__=='__main__':main()
