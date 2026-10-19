import sys,bisect,re
L=[]
for l in open('/home/claude/bin/syms_sorted.txt'):
    p=l.rstrip("\n").split(" ",2)
    if not p[0]: continue
    if len(p)<3: continue
    L.append((int(p[0],16),p[2]))
L.sort(); A=[x[0] for x in L]
def a2s(a):
    i=bisect.bisect_right(A,a)-1
    return "%s+0x%x"%(L[i][1][:150],a-L[i][0])
for line in sys.stdin:
    m=re.search(r'@ ([0-9a-f]{8})',line)
    if m and line.strip().startswith('<-'):
        print(line.rstrip()+"   "+a2s(int(m.group(1),16)-0x100000))
    else: print(line.rstrip())
