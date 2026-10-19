import sys
out=open('src/fonts_data.c','w')
for name,path in [('font_cinzel','fonts/Cinzel-SemiBold.ttf'),('font_lora','fonts/Lora-Regular.ttf'),('font_lora_bold','fonts/Lora-SemiBold.ttf')]:
    d=open(path,'rb').read()
    out.write('const unsigned char %s[] = {\n'%name)
    for i in range(0,len(d),24): out.write(','.join(str(b) for b in d[i:i+24])+',\n')
    out.write('};\nconst unsigned int %s_len = %d;\n'%(name,len(d)))
