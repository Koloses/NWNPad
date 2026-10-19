#!/usr/bin/env python3
# generates src/sigs.inc: first 8 bytes of every hooked/called function in the supported nwmain.exe
import re,sys,pefile
exe=sys.argv[1]; out=sys.argv[2]; srcs=sys.argv[3:]
pe=pefile.PE(exe,fast_load=True); img=pe.get_memory_mapped_image(); base=pe.OPTIONAL_HEADER.ImageBase
vas=[]
for s in srcs:
    for m in re.finditer(r'(\w+)\s*(?:=\s*|\()(0x14[0-9a-fA-F]{7})\b',open(s).read()):
        vas.append((m.group(1),int(m.group(2),16)))
seen=set(); lines=[]
for n,va in vas:
    if va in seen: continue
    seen.add(va)
    b=img[va-base:va-base+8]
    lines.append('    {0x%x, "%s", {%s}},'%(va,n,','.join('0x%02x'%x for x in b)))
open(out,'w').write('static const Sig g_sigs[] = {\n'+'\n'.join(lines)+'\n};\n')
print(len(lines),'sigs')
