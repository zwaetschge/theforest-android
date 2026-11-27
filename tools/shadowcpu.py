#!/usr/bin/env python3
"""Give every lifted function a local shadow copy of the CPU registers it uses.

The lifter writes guest registers and flags straight into the CPU struct. Guest
memory is reached through integer-derived pointers, so clang must assume any
guest load or store may touch those fields: nothing stays in a host register
and no flag computation is ever dead. With a local copy whose address never
escapes, SROA turns the fields into SSA values; dead flags vanish and registers
are allocated normally.

Sync points: every call that hands the CPU to code outside this function
(L_xxx(c), dispatch(c, ...), dispatch_jmp(c, ...)) stores the used fields back
before and reloads them after; every `return` stores them back. Fields a
function never touches are left in the real struct, so callers and callees still
see them. Functions with C++ EH landing pads (setjmp) are left alone.

usage: shadowcpu.py <gendir> [skip_units.txt]
Idempotent: transformed functions are recognised and skipped.
"""
import os, re, sys

gen = sys.argv[1]
skip_units = set()
if len(sys.argv) > 2 and os.path.exists(sys.argv[2]):
    skip_units = {l.strip() for l in open(sys.argv[2]) if l.strip() and not l.startswith('#')}

GPRS = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
        'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
FLAGS = ['cf', 'zf', 'sf', 'of', 'pf', 'af']
ALWAYS = {'rsp', 'rip'} | set(FLAGS)
HELPER_FIELDS = {'do_cpuid': {'rax', 'rbx', 'rcx', 'rdx'}}

FIELD_RE = re.compile(r'\bc->(r[a-z0-9]+|cf|zf|sf|of|pf|af|mxcsr|xmm\[(\d+)\]|mm\[(\d+)\])')
CALL_RE = re.compile(r'\b(L_[0-9A-F]{12})\(c\)|\b(dispatch|dispatch_jmp)\(c,')
FUNC_RE = re.compile(r'^void (L_[0-9A-F]{12})\(CPU \*c\)\n\{\n(.*?)^\}\n', re.M | re.S)


def transform(m):
    name, body = m.group(1), m.group(2)
    if 'ES3_EH_ENTER' in body or 'c_shadow' in body:
        return m.group(0)
    # giant straight-line init functions: no gain, and clang -O1 crawls on them
    if body.count('\n') > 6000:
        return m.group(0)
    used = set(ALWAYS)
    xmms = set()
    mms = set()
    for f in FIELD_RE.finditer(body):
        if f.group(2) is not None:
            xmms.add(int(f.group(2)))
        elif f.group(3) is not None:
            mms.add(int(f.group(3)))
        else:
            used.add(f.group(1))
    for h, fs in HELPER_FIELDS.items():
        if h + '(c' in body:
            used |= fs
    fields = [g for g in GPRS if g in used] + (['rip'] if 'rip' in used else []) + \
             [f for f in FLAGS if f in used] + (['mxcsr'] if 'mxcsr' in used else [])
    load = ' '.join('c_shadow.%s = c_->%s;' % (f, f) for f in fields)
    load += ''.join(' c_shadow.xmm[%d] = c_->xmm[%d];' % (i, i) for i in sorted(xmms))
    load += ''.join(' c_shadow.mm[%d] = c_->mm[%d];' % (i, i) for i in sorted(mms))
    # Store-back variants: a real call or `ret` does not carry flags (Win64 ABI),
    # so flags are written back only where control flows on inside the same
    # original code (tail jumps into split functions, fall-through extents).
    store_full = ' '.join('c_->%s = c_shadow.%s;' % (f, f) for f in fields)
    store_full += ''.join(' c_->xmm[%d] = c_shadow.xmm[%d];' % (i, i) for i in sorted(xmms))
    store_full += ''.join(' c_->mm[%d] = c_shadow.mm[%d];' % (i, i) for i in sorted(mms))
    nf = [f for f in fields if f not in FLAGS]
    store_call = ' '.join('c_->%s = c_shadow.%s;' % (f, f) for f in nf)
    store_call += ''.join(' c_->xmm[%d] = c_shadow.xmm[%d];' % (i, i) for i in sorted(xmms))
    store_call += ''.join(' c_->mm[%d] = c_shadow.mm[%d];' % (i, i) for i in sorted(mms))

    lines = body.split('\n')
    out_lines = []
    for ln in lines:
        is_call = 'push64(c, ' in ln
        # direct calls / jumps
        def dsub(cm):
            st = store_call if is_call else store_full
            return '{ %s %s(c_); %s }' % (st, cm.group(1), load)
        ln = re.sub(r'\b(L_[0-9A-F]{12})\(c\);', dsub, ln)
        # dispatch(c, X); dispatch_jmp(c, X);   (argument may nest parentheses)
        pat = re.compile(r'\b(dispatch|dispatch_jmp)\(c, ')
        o = []
        i = 0
        while True:
            dm = pat.search(ln, i)
            if not dm:
                o.append(ln[i:])
                break
            o.append(ln[i:dm.start()])
            j = dm.end()
            depth = 1
            while depth:
                ch = ln[j]
                if ch == '(':
                    depth += 1
                elif ch == ')':
                    depth -= 1
                j += 1
            arg = ln[dm.end():j - 1]
            k = j
            while ln[k] == ' ':
                k += 1
            assert ln[k] == ';', ln
            st = store_call if (is_call and dm.group(1) == 'dispatch') else store_full
            o.append('{ uint64_t _dt = (%s); %s %s(c_, _dt); %s }' % (arg, st, dm.group(1), load))
            i = k + 1
        ln = ''.join(o)
        # `ret` (c->rsp += 8; return;) carries no flags; other returns follow a
        # transfer that already stored everything
        ln = re.sub(r'\breturn;', '{ %s return; }' % store_call, ln)
        out_lines.append(ln)
    body2 = '\n'.join(out_lines)
    store = store_full
    head = 'void %s(CPU *c_)\n{\n    CPU c_shadow; CPU *const c = &c_shadow;\n    %s\n' % (name, load)
    tail = '    %s\n}\n' % store       # falling off the end (should not happen, but keep state)
    return head + body2 + tail


tot = 0
for fn in sorted(os.listdir(gen)):
    if not (fn.startswith('recomp_funcs_') and fn.endswith('.c')):
        continue
    if fn[:-2] in skip_units:
        continue
    p = os.path.join(gen, fn)
    s = open(p).read()
    s2, n = FUNC_RE.subn(transform, s)
    if s2 != s:
        open(p, 'w').write(s2)
        tot += n
print('shadow CPU: %d functions' % tot)
