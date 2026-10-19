import json,collections,pickle,math
M,src=pickle.load(open('/home/claude/gh/match.pkl','rb'))
def load(p):
    F={}
    for l in open(p):
        o=json.loads(l); F[o['a']]=o
    return F
L=load('/home/claude/gh/lin_funcs.jsonl'); W=load('/home/claude/gh/win_funcs.jsonl')
R={w:l for l,w in M.items()}
def callers(F):
    d=collections.defaultdict(set)
    for a,o in F.items():
        for c in o['c']:
            if isinstance(c,int): d[c].add(a)
    return d
LC=callers(L); WC=callers(W)
def feat_l(a):
    o=L[a]
    return (set(('m',M[c]) for c in o['c'] if isinstance(c,int) and c in M),
            set(o['s']), collections.Counter(o['i']), o['ni'],
            set(('m',M[c]) for c in LC.get(a,()) if c in M))
def feat_w(a):
    o=W[a]
    return (set(('m',c) for c in o['c'] if isinstance(c,int) and c in R),
            set(o['s']), collections.Counter(o['i']), o['ni'],
            set(('m',c) for c in WC.get(a,()) if c in R))
def jac(a,b):
    if not a and not b: return None
    return len(a&b)/max(1,len(a|b))
def cjac(a,b):
    if not a and not b: return None
    inter=sum((a&b).values()); uni=sum((a|b).values())
    return inter/max(1,uni)
def sim(fl,fw):
    sc=[];wt=[]
    for x,y,w in ((fl[0],fw[0],3),(fl[1],fw[1],3),(fl[4],fw[4],2)):
        j=jac(x,y)
        if j is not None: sc.append(j*w); wt.append(w)
    j=cjac(fl[2],fw[2])
    if j is not None: sc.append(j*1.5); wt.append(1.5)
    if not wt: return 0
    s=sum(sc)/sum(wt)
    r=min(fl[3],fw[3])/max(1,max(fl[3],fw[3]))
    return s*(0.5+0.5*r)
for it in range(12):
    cand=collections.defaultdict(dict)
    for l,w in list(M.items()):
        ul=[c for c in L[l]['c'] if isinstance(c,int) and c not in M]+[c for c in LC.get(l,()) if c not in M]
        uw=[c for c in W[w]['c'] if isinstance(c,int) and c not in R]+[c for c in WC.get(w,()) if c not in R]
        ul=set(ul); uw=set(uw)
        if not ul or not uw or len(ul)*len(uw)>400: continue
        fls={a:feat_l(a) for a in ul}; fws={a:feat_w(a) for a in uw}
        for a in ul:
            for b in uw:
                s=sim(fls[a],fws[b])
                if s>cand[a].get(b,0): cand[a][b]=s
    # choose mutual best with margin
    bestw={}
    for a,d in cand.items():
        for b,s in d.items():
            if s>bestw.get(b,(0,None))[0]: bestw[b]=(s,a)
    n=0
    for a,d in cand.items():
        items=sorted(d.items(),key=lambda x:-x[1])
        if not items: continue
        b,s=items[0]
        s2=items[1][1] if len(items)>1 else 0
        if s>=0.45 and s-s2>=0.12 and bestw[b][1]==a and a not in M and b not in R:
            M[a]=b; R[b]=a; src[a]='fuzzy%d:%.2f'%(it,s); n+=1
    print('iter',it,'added',n,'total',len(M))
    if n==0: break
pickle.dump((M,src),open('/home/claude/gh/match.pkl','wb'))
