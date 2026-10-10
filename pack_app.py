#!/usr/bin/env python3
import argparse, struct, zlib
p=argparse.ArgumentParser()
p.add_argument('infile'); p.add_argument('outfile')
p.add_argument('--name',default='app'); p.add_argument('--ver',default='1.0')
p.add_argument('--api-min',type=int,default=1); p.add_argument('--vma',type=lambda x:int(x,0),required=True)
a=p.parse_args(); code=open(a.infile,'rb').read()
if not code or len(code)>0x2000: raise SystemExit(f'invalid code size {len(code)} (max 8192)')
def field(s,n):
 b=s.encode('ascii')[:n-1]; return b+b'\0'*(n-len(b))
h=struct.pack('<4sHBBIIHH16s16sII4s',b'FAP1',1,1,a.api_min,len(code),zlib.crc32(code)&0xffffffff,0,1,field(a.name,16),field(a.ver,16),a.vma,0,b'\0'*4)
assert len(h)==64
open(a.outfile,'wb').write(h+code)
print(f"{a.outfile}: name={a.name!r} ver={a.ver!r} abi=1 api>={a.api_min} vma=0x{a.vma:08x} code={len(code)} B crc32=0x{zlib.crc32(code)&0xffffffff:08x} -> blob {64+len(code)} B")
