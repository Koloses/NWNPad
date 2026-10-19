import json,re,collections,difflib,sys,pickle
def load(p):
    F={}
    for l in open(p):
        try: o=json.loads(l)
        except Exception as e: continue
        F[o['a']]=o
    return F
L=load('/home/claude/gh/lin_funcs.jsonl'); W=load('/home/claude/gh/win_funcs.jsonl')
print('funcs',len(L),len(W))
# linux names
lname={a:o['n'] for a,o in L.items()}
byname=collections.defaultdict(list)
for a,n in lname.items(): byname[n].append(a)
M={}   # lin->win
R={}   # win->lin
src={}
def add(l,w,why):
    if l in M or w in R: return False
    M[l]=w; R[w]=l; src[l]=why; return True
# Anchor 0: exported names
for a,o in W.items():
    n=o['n']
    if n.startswith('FUN_') or '::' not in n: continue
    c=byname.get(n,[])
    if len(c)==1: add(c[0],a,'export')
print('exports',len(M))
# Anchor A: FUNCSIG
sig=re.compile(r'__cdecl ([A-Za-z_0-9:<>]+)\(([^)]*)\)')
cnt=0
for a,o in W.items():
    for s in o['s']:
        m=sig.search(s)
        if m:
            nm=m.group(1)
            cands=byname.get(nm,[])
            if len(cands)==1:
                if add(cands[0],a,'funcsig'): cnt+=1
            break
print('funcsig',cnt)
import json as _j
WSL=_j.load(open('/home/claude/gh/win_srcloc.json'))
cnt=0
for a,names in WSL.items():
    a=int(a); ns=set()
    for x in names:
        m=sig.search(x)
        if m: ns.add(m.group(1))
    if len(ns)==1:
        nm=ns.pop(); cands=byname.get(nm,[])
        if len(cands)==1 and add(cands[0],a,'srcloc'): cnt+=1
print('srcloc',cnt)
# Anchor B: unique strings
def strindex(F):
    d=collections.defaultdict(set)
    for a,o in F.items():
        for s in set(o['s']):
            if len(s)>=6: d[s].add(a)
    return d
LS=strindex(L); WS=strindex(W)
votes=collections.Counter()
for s,ls in LS.items():
    ws=WS.get(s)
    if ws and len(ls)==1 and len(ws)==1:
        votes[(next(iter(ls)),next(iter(ws)))]+=1
# resolve conflicts: keep best per l and per w
bestl={};bestw={}
for (l,w),v in votes.items():
    if v>bestl.get(l,(0,None))[0]: bestl[l]=(v,w)
    if v>bestw.get(w,(0,None))[0]: bestw[w]=(v,l)
cnt=0
for (l,w),v in votes.items():
    if bestl[l][1]==w and bestw[w][1]==l:
        if add(l,w,'str%d'%v): cnt+=1
print('strings',cnt)
# multi-string signature: set of strings identical (non unique), tie-break by unique set
sets=collections.defaultdict(list)
for a,o in L.items():
    ss=tuple(sorted(set(x for x in o['s'] if len(x)>=4)))
    if len(ss)>=2: sets[ss].append(('L',a))
for a,o in W.items():
    ss=tuple(sorted(set(x for x in o['s'] if len(x)>=4)))
    if len(ss)>=2: sets[ss].append(('W',a))
cnt=0
for ss,lst in sets.items():
    ls=[a for t,a in lst if t=='L']; ws=[a for t,a in lst if t=='W']
    if len(ls)==1 and len(ws)==1:
        if add(ls[0],ws[0],'strset'): cnt+=1
print('strset',cnt)
def ccalls(F,a):
    return [c for c in F[a]['c'] if isinstance(c,int)]
def ext(F,a):
    return [c for c in F[a]['c'] if isinstance(c,str)]
# propagation
def propagate(rounds=30):
    for r in range(rounds):
        prop=collections.Counter()
        for l,w in list(M.items()):
            lc=ccalls(L,l); wc=ccalls(W,w)
            if not lc or not wc: continue
            lt=[('m%d'%M[c]) if c in M else '?' for c in lc]
            wt=[('m%d'%c) if c in R else '?' for c in wc]
            sm=difflib.SequenceMatcher(None,lt,wt,autojunk=False)
            for tag,i1,i2,j1,j2 in sm.get_opcodes():
                if tag=='replace' and (i2-i1)==(j2-j1):
                    for k in range(i2-i1):
                        lcand=lc[i1+k]; wcand=wc[j1+k]
                        if lcand not in M and wcand not in R:
                            prop[(lcand,wcand)]+=1
            # unique-unknown callee sets: if exactly one unknown each side
            ul=set(c for c in lc if c not in M); uw=set(c for c in wc if c not in R)
            if len(ul)==1 and len(uw)==1:
                prop[(next(iter(ul)),next(iter(uw)))]+=2
        # callers propagation: for matched callee, if unknown callers sets size 1 each
        if not prop: break
        bl={};bw={}
        for (l,w),v in prop.items():
            if v>bl.get(l,(0,None))[0]: bl[l]=(v,w)
            elif v==bl.get(l,(0,None))[0] and bl[l][1]!=w: bl[l]=(v,None)
            if v>bw.get(w,(0,None))[0]: bw[w]=(v,l)
            elif v==bw.get(w,(0,None))[0] and bw[w][1]!=l: bw[w]=(v,None)
        n=0
        for (l,w),v in prop.items():
            if bl[l][1]==w and bw[w][1]==l:
                # size sanity
                li=L[l]['ni']; wi=W[w]['ni']
                if wi>0 and li>0 and (li/wi>6 or wi/li>6) and v<3: continue
                if add(l,w,'prop%d'%r): n+=1
        print('round',r,'added',n,'total',len(M))
        if n==0: break
propagate()
# callers propagation
def callers(F):
    d=collections.defaultdict(set)
    for a,o in F.items():
        for c in o['c']:
            if isinstance(c,int): d[c].add(a)
    return d
LC=callers(L); WC=callers(W)
for it in range(10):
    n=0
    for l,w in list(M.items()):
        ul=[c for c in LC.get(l,()) if c not in M]; uw=[c for c in WC.get(w,()) if c not in R]
        if len(ul)==1 and len(uw)==1:
            if add(ul[0],uw[0],'caller'): n+=1
    print('callers',n)
    propagate(5)
    if n==0: break
pickle.dump((M,src),open('/home/claude/gh/match.pkl','wb'))
print('TOTAL',len(M))
