import json,pickle,sys,re,collections
M,src=pickle.load(open('/home/claude/gh/match.pkl','rb'))
L={};LN={}
for l in open('/home/claude/gh/lin_funcs.jsonl'):
    o=json.loads(l); L[o['a']]=o; LN[o['a']]=o['n']
LC=collections.defaultdict(list)
for a,o in L.items():
    for c in o['c']:
        if isinstance(c,int): LC[c].append(a)
for pat in sys.argv[1:]:
    for a,n in LN.items():
        if re.search(pat,n):
            print('##',n,hex(a),'W=',hex(M[a]+0x140000000) if a in M else '-')
            for c in sorted(set(LC[a])):
                print('   <-',LN[c][:80],'W=',hex(M[c]+0x140000000) if c in M else '-')
            print('   ->',[ (LN.get(c,c)[:40], hex(M[c]+0x140000000) if c in M else '-') for c in L[a]['c'] if isinstance(c,int)][:30])
