#!/usr/bin/env python3
"""
dt_corpus.py - scan a whole PE (by its function catalog) and collect every
distinct instruction FORM, for the Linux port of pcrecomp's difftest64.

A form is the mnemonic plus the operand string with memory displacements
normalised away (`[rcx + 0x10]` and `[rcx + 0x48]` are the same test); the
first encoding seen for each form is kept, with a frequency count, so the case
selector can weight by how often the game actually uses it.

Usage: dt_corpus.py <pe> <funcs.txt> <out.json> [--jobs N]
"""
import json
import os
import re
import sys
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'third_party', 'pcrecomp', 'tools', 'lift'))

import lift64_cpu                                   # noqa: E402
from lift64_cpu import load_bounds                  # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64    # noqa: E402

_DISP_RE = re.compile(r'([+-]) 0x[0-9a-f]+\]')
_ABS_RE = re.compile(r'\[0x[0-9a-f]+\]')

G = {}


def form_key(mn, ops):
    k = _DISP_RE.sub(r'\1 D]', ops)
    k = _ABS_RE.sub('[D]', k)
    return mn + ' ' + k


def _init(pe):
    data = open(pe, 'rb').read()
    b, soi, secs = lift64_cpu._pe_sections(data)
    G['data'], G['base'], G['secs'] = data, b, secs


def read_va(va, n):
    data, b = G['data'], G['base']
    for (nm, vsa, vs, ra, rs) in G['secs']:
        lo = b + vsa
        if lo <= va < lo + max(vs, rs):
            return data[ra + (va - lo): ra + (va - lo) + n]
    raise KeyError(hex(va))


def _scan(chunk):
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = False                    # forms only; detail is recomputed later
    forms = {}
    n = 0
    for va, size in chunk:
        try:
            code = read_va(va, size)
        except KeyError:
            continue
        for (addr, sz, mn, ops) in md.disasm_lite(code, va):
            n += 1
            k = form_key(mn, ops)
            e = forms.get(k)
            if e is None:
                forms[k] = [1, code[addr - va: addr - va + sz].hex(), addr]
            else:
                e[0] += 1
    return n, forms


def main():
    argv = sys.argv[1:]
    jobs = os.cpu_count() or 4
    if '--jobs' in argv:
        i = argv.index('--jobs')
        jobs = int(argv[i + 1])
        del argv[i:i + 2]
    pe, funcs, out = argv[:3]
    bounds = load_bounds(funcs)
    items = sorted((va, sz) for va, (sz, _n) in bounds.items() if sz > 0)
    nch = jobs * 8
    chunks = [items[i::nch] for i in range(nch)]
    total = 0
    forms = {}
    with Pool(jobs, initializer=_init, initargs=(pe,)) as p:
        for n, f in p.imap_unordered(_scan, chunks):
            total += n
            for k, (c, enc, a) in f.items():
                e = forms.get(k)
                if e is None:
                    forms[k] = [c, enc, a]
                else:
                    e[0] += c
    print('[corpus] %s: %d functions, %d instructions, %d distinct forms'
          % (os.path.basename(pe), len(items), total, len(forms)))
    with open(out, 'w') as f:
        json.dump({'pe': pe, 'instructions': total, 'forms': forms}, f)


if __name__ == '__main__':
    main()
