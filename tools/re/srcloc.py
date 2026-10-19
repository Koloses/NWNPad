# For each function's data refs, try to read a tracy SourceLocationData {name,function,file,line,color} and extract function/file strings
import json,pefile,sys,collections
from elftools.elf.elffile import ELFFile
class PEMem:
    def __init__(s,p):
        s.pe=pefile.PE(p,fast_load=True); s.base=s.pe.OPTIONAL_HEADER.ImageBase
        s.img=s.pe.get_memory_mapped_image()
    def rd(s,rva,n):
        if rva<0 or rva+n>len(s.img): return None
        return s.img[rva:rva+n]
    def ptr2rva(s,p): return p-s.base
class ELFMem:
    def __init__(s,p):
        f=open(p,'rb'); s.e=ELFFile(f); s.segs=[]
        for seg in s.e.iter_segments():
            if seg['p_type']=='PT_LOAD': s.segs.append((seg['p_vaddr'],seg['p_filesz'],seg.data()))
        # relocations for pointers (PIE): build map from RELA R_X86_64_RELATIVE
        s.rel={}
        for sec in s.e.iter_sections():
            if sec.header['sh_type']=='SHT_RELA':
                for r in sec.iter_relocations():
                    if r['r_info_type']==8: s.rel[r['r_offset']]=r['r_addend']
    def rd(s,va,n):
        for v,sz,d in s.segs:
            if v<=va and va+n<=v+sz: return d[va-v:va-v+n]
        return None
    def ptr2rva(s,p): return p
def cstr(m,rva):
    b=m.rd(rva,200)
    if not b: return None
    i=b.find(b'\0')
    if i<=0: return None
    t=b[:i]
    if all(32<=c<127 for c in t): return t.decode()
    return None
def run(mem,funcs,out,islin,base_off):
    res={}
    for l in open(funcs):
        o=json.loads(l)
        names=[]
        for d in o['d']:
            va=d+base_off
            raw=mem.rd(va,16)
            if not raw: continue
            p0=int.from_bytes(raw[0:8],'little'); p1=int.from_bytes(raw[8:16],'little')
            if islin:
                p0=mem.rel.get(va,p0); p1=mem.rel.get(va+8,p1)
            f=cstr(mem,mem.ptr2rva(p1)) if p1 else None
            if f and ('::' in f or f.isidentifier()) and len(f)>3:
                n0=cstr(mem,mem.ptr2rva(p0)) if p0 else None
                names.append(f)
        if names: res[o['a']]=names
    json.dump(res,open(out,'w'))
    print(out,len(res))
if sys.argv[1]=='win':
    m=PEMem('/home/claude/bin/nwmain.exe'); run(m,'/home/claude/gh/win_funcs.jsonl','/home/claude/gh/win_srcloc.json',False,0)
else:
    m=ELFMem('/home/claude/bin/nwmain-linux'); run(m,'/home/claude/gh/lin_funcs.jsonl','/home/claude/gh/lin_srcloc.json',True,0)
