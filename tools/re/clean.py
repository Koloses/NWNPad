import re,sys
s=open(sys.argv[1]).read()
s=re.sub(r'\n\s*\n','\n',s)
out=[]
for l in s.split('\n'):
    if re.match(r'\s*(undefined\d*|ulong|long|uint|int|char|byte|bool|float|double|ushort|short|code|uchar|CExoString|CNW\w+|C\w+|Vector|Explicit\w+|size_t|void)\s+\**[a-zA-Z_]\w*( \[\d+\])?;$',l): continue
    if 'WARNING' in l: continue
    out.append(l)
print('\n'.join(out))
