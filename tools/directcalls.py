#!/usr/bin/env python3
"""Rewrite `dispatch(c, 0x<va>ull)` to a direct `L_<va>(c)` call when <va> is a lifted entry.

Keeps dispatch() for imports, sentinels, unknown targets and anything listed in
config/keep_dispatch.txt (functions the runtime may override). Idempotent.
usage: directcalls.py <gendir> [keep_dispatch.txt]
"""
import os, re, sys
gen = sys.argv[1]
keep = set()
if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
    for l in open(sys.argv[2]):
        l = l.split('#')[0].strip()
        if l: keep.add(int(l, 16))
known = set()
for l in open(os.path.join(gen, 'recomp_dispatch.c')):
    m = re.match(r'\s*\{ 0x([0-9A-F]+)ull, L_', l)
    if m: known.add(int(m.group(1), 16))
RE = re.compile(r'dispatch(?:_jmp)?\(c, 0x([0-9A-Fa-f]+)ull\)')
def sub(m):
    va = int(m.group(1), 16)
    if va in known and va not in keep:
        return 'L_%012X(c)' % va
    return m.group(0)
tot = 0
for fn in sorted(os.listdir(gen)):
    if not (fn.startswith('recomp_funcs_') and fn.endswith('.c')): continue
    p = os.path.join(gen, fn)
    s = open(p).read()
    n = len(RE.findall(s))
    s2 = RE.sub(sub, s)
    left = len(RE.findall(s2))
    tot += n - left
    if s2 != s: open(p, 'w').write(s2)
print('direct calls:', tot)
