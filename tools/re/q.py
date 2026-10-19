import pickle,json,sys,re
M,src=pickle.load(open('/home/claude/gh/match.pkl','rb'))
L={}
for l in open('/home/claude/gh/lin_funcs.jsonl'):
    o=json.loads(l); L[o['a']]=o['n']
for pat in sys.argv[1:]:
    p=re.compile(pat)
    for a,n in sorted(L.items(),key=lambda x:x[1]):
        if p.search(n):
            w=M.get(a)
            print("%-70s L=%08x  W=%s  %s"%(n[:70],a, ('%08x'%(w+0x140000000)) if w is not None else '-', src.get(a,'')))
