#!/usr/bin/env python3
import re,sys
log=open('/tmp/serial.log',errors='replace').read().replace('\r','').split('\n')
man_full=set(l for l in open('/home/claude/t/tree_manifest_full.txt',encoding='utf-8').read().split('\n') if l)
man_ntfs=set(l for l in open('/home/claude/t/tree_manifest_ntfs.txt',encoding='utf-8').read().split('\n') if l)
cur=None; got={}; bad={}
for l in log:
    m=re.match(r'\[TREE\] BEGIN (\S+) (\S+)',l)
    if m: cur=m.group(1); got[cur]=set(); bad[cur]=[]; fsn=m.group(2); continue
    if l.startswith('[TREE] END'): cur=None; continue
    if cur and l.startswith('[TREE] '):
        t=l[7:]
        if t.startswith(('F ','D ')): got[cur].add(t)
        else: bad[cur].append(t)
exp={'disk0':man_full,'disk1':man_full,'disk2':man_ntfs}
for d in sorted(got):
    g={x for x in got[d] if 'lost+found' not in x}; e=exp.get(d,set())
    if d=='disk2': g={x for x in g if x.startswith('F ')}
    miss=e-g; extra=g-e
    print(f"{d}: expected={len(e)} got={len(g)} ok={len(e&g)} missing={len(miss)} wrong/extra={len(extra)} type/missing-msgs={len(bad[d])}")
    for x in sorted(miss)[:4]: print('   MISSING:',x[:110])
    for x in sorted(extra)[:3]: print('   EXTRA  :',x[:110])
    for x in bad[d][:3]: print('   MSG    :',x[:110])
