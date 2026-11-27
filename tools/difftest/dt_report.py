#!/usr/bin/env python3
"""
dt_report.py - classify difftest64_linux mismatch records and write report.txt.

Every recorded mismatching trial is classified:
  BUG    a defined part of the architectural state differs
  UNDEF  only state the ISA leaves undefined for THIS input differs (OF after a
         multi-bit shift/rotate, CF after shl/shr by >= width, shld/shrd count
         > width) - the static per-mnemonic masks are applied in C already
  NAN    only lanes where native and lifted are BOTH NaN differ (payload/sign
         of the propagated NaN: operand order of the C expression)
A case is a FAIL if any trial is BUG.

Usage: dt_report.py <outdir> [<outdir2> ...] --out report.txt
"""
import argparse
import json
import math
import os
import re
import struct
import sys
from collections import defaultdict, Counter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'third_party', 'pcrecomp', 'tools', 'lift'))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64           # noqa: E402
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM  # noqa: E402

GN = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
      'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
FL = [(0, 'CF'), (2, 'PF'), (4, 'AF'), (6, 'ZF'), (7, 'SF'), (11, 'OF')]
FMASK = sum(1 << b for b, _ in FL)
ST_FMT = '<16QQQ256s8Q'
ST_SIZE = struct.calcsize(ST_FMT)
REC_FMT = '<4i'
REC_SIZE = 16 + 3 * ST_SIZE + 64
assert ST_SIZE == 464


def parse_state(b):
    v = struct.unpack(ST_FMT, b)
    gpr = list(v[:16])
    flags = v[16]
    xb = v[18]
    xmm = [xb[16 * k:16 * k + 16] for k in range(16)]
    mm = list(v[19:27])
    return dict(gpr=gpr, flags=flags, xmm=xmm, mm=mm)


def read_records(path):
    out = defaultdict(list)
    if not os.path.exists(path):
        return out
    data = open(path, 'rb').read()
    for off in range(0, len(data) - REC_SIZE + 1, REC_SIZE):
        idx, trial, kind, aux = struct.unpack_from(REC_FMT, data, off)
        p = off + 16
        st = [parse_state(data[p + k * ST_SIZE: p + (k + 1) * ST_SIZE]) for k in range(3)]
        p += 3 * ST_SIZE
        out[idx].append(dict(trial=trial, kind=kind, aux=aux, inp=st[0], nat=st[1], lif=st[2],
                             memnat=data[p:p + 32], memlif=data[p + 32:p + 64]))
    return out


def f32s(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def f64s(u):
    return struct.unpack('<d', struct.pack('<Q', u))[0]


def lanes32(b):
    return list(struct.unpack('<4I', b))


def lanes64(b):
    return list(struct.unpack('<2Q', b))


def isnan32(u):
    return (u & 0x7F800000) == 0x7F800000 and (u & 0x7FFFFF) != 0


def isnan64(u):
    return (u & 0x7FF0000000000000) == 0x7FF0000000000000 and (u & 0xFFFFFFFFFFFFF) != 0


def fmt_f32(u):
    if isnan32(u):
        return '%sNaN(%s,%06x)' % ('-' if u >> 31 else '', 'q' if u & 0x400000 else 's', u & 0x7FFFFF)
    x = f32s(u)
    return repr(x)


def fmt_f64(u):
    if isnan64(u):
        return '%sNaN(%s)' % ('-' if u >> 63 else '', 'q' if u & (1 << 51) else 's')
    return repr(f64s(u))


def xmm_str(b, view):
    """Lanes in INDEX order (lane 0 first) for both the hex and the decoded values."""
    if view == 'f32':
        L = lanes32(b)
        return 'u32[0..3] %s  = f32 (%s)' % (' '.join('%08x' % x for x in L), ', '.join(fmt_f32(x) for x in L))
    if view == 'f64':
        L = lanes64(b)
        return 'u64[0..1] %s  = f64 (%s)' % (' '.join('%016x' % x for x in L), ', '.join(fmt_f64(x) for x in L))
    if view == 'i16':
        L = struct.unpack('<8H', b)
        return 'u16[0..7] %s' % ' '.join('%04x' % x for x in L)
    if view == 'i8':
        return 'u8[0..15] %s' % b.hex(' ')
    L = lanes32(b)
    return 'u32[0..3] %s' % ' '.join('%08x' % x for x in L)


def flag_str(f):
    return ' '.join(n for b, n in FL if f >> b & 1) or '-'


def lane_view(mn):
    base = mn.split()[-1]
    if base in ('cvtss2sd', 'cvtps2pd', 'cvtdq2pd', 'cvtsi2sd') or base.endswith('pd') or base.endswith('sd'):
        if base not in ('cvtsd2ss', 'cvtpd2ps', 'cvtpd2dq', 'cvttpd2dq'):
            return 'f64'
    if base.endswith('ps') or base.endswith('ss') or base in ('cvtsd2ss', 'cvtpd2ps'):
        return 'f32'
    if base[0] == 'p' and base[-1] == 'w':
        return 'i16'
    if base[0] == 'p' and base[-1] == 'b':
        return 'i8'
    return 'u32'


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True


def decode(enc):
    return list(md.disasm(bytes.fromhex(enc), 0x140000000))[0]


def shift_count(ins, inp):
    """(masked count, width) for shift/rotate forms, else None."""
    base = ins.mnemonic.split()[-1]
    if base not in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        return None
    ops = ins.operands
    w = ops[0].size * 8
    cop = ops[-1] if len(ops) >= (3 if base in ('shld', 'shrd') else 2) else None
    if cop is None:
        cnt = 1
    elif cop.type == X86_OP_IMM:
        cnt = cop.imm & 0xFF
    else:
        cnt = inp['gpr'][1] & 0xFF
    return cnt & (63 if w == 64 else 31), w


def classify(case, rec):
    """-> (class, list of diff descriptions)."""
    inp, nat, lif = rec['inp'], rec['nat'], rec['lif']
    ins = decode(case['enc'])
    base = ins.mnemonic.split()[-1]
    if rec['kind'] == 2:
        return 'BUG', [('lifted-fault', 'signal %d' % rec['aux'], '', '')]
    diffs = []
    undef_ok = set()
    sc = shift_count(ins, inp)
    if sc:
        cnt, w = sc
        if cnt != 1:
            undef_ok.add('OF')
        if base in ('shl', 'sal', 'shr') and cnt >= w:
            undef_ok.add('CF')
        if base in ('shld', 'shrd') and cnt > w:
            undef_ok |= {'CF', 'OF', 'SF', 'ZF', 'PF', 'AF', 'dst'}
    benign_nan = True
    for k in range(16):
        if k == 4:
            continue
        if nat['gpr'][k] != lif['gpr'][k]:
            diffs.append(('gpr', GN[k], nat['gpr'][k], lif['gpr'][k]))
    fm = FMASK & ~case['undef']
    if (nat['flags'] ^ lif['flags']) & fm:
        d = (nat['flags'] ^ lif['flags']) & fm
        for b, n in FL:
            if d >> b & 1:
                diffs.append(('flag', n, nat['flags'] >> b & 1, lif['flags'] >> b & 1))
    view = lane_view(ins.mnemonic)
    approx = base in ('rcpss', 'rcpps', 'rsqrtss', 'rsqrtps')
    for k in range(16):
        a, b = nat['xmm'][k], lif['xmm'][k]
        if a == b:
            continue
        if approx:
            ok = True
            for l in range(4):
                x, y = lanes32(a)[l], lanes32(b)[l]
                if x == y or (isnan32(x) and isnan32(y)):
                    continue
                if base.endswith('ss') and l:
                    ok = False
                    continue
                fx, fy = f32s(x), f32s(y)
                if math.isinf(fx) or math.isinf(fy) or fx == 0:
                    ok = ok and fx == fy
                else:
                    ok = ok and abs(fy - fx) / abs(fx) <= 1.5 / 4096
            if ok:
                continue
        diffs.append(('xmm', 'xmm%d' % k, a, b))
        # NaN-only?
        if view == 'f64':
            for x, y in zip(lanes64(a), lanes64(b)):
                if x != y and not (isnan64(x) and isnan64(y)):
                    benign_nan = False
        else:
            for x, y in zip(lanes32(a), lanes32(b)):
                if x != y and not (isnan32(x) and isnan32(y)):
                    benign_nan = False
    for k in range(8):
        if nat['mm'][k] != lif['mm'][k]:
            diffs.append(('mm', 'mm%d' % k, nat['mm'][k], lif['mm'][k]))
    if rec['aux'] >= 0 and rec['memnat'] != rec['memlif']:
        o = rec['aux'] & ~15
        if o + 32 > 0x4000:
            o = 0x4000 - 32
        diffs.append(('mem', 'mem scratch+%#x..+%#x (first diff +%#x)' % (o, o + 31, rec['aux']),
                      rec['memnat'], rec['memlif']))
    if not diffs:
        return 'PASS', []
    kinds = set(d[0] for d in diffs)
    if all(d[0] == 'flag' and d[1] in undef_ok for d in diffs):
        return 'UNDEF', diffs
    if 'dst' in undef_ok and kinds <= {'flag', 'gpr', 'mem'}:
        return 'UNDEF', diffs
    if kinds == {'xmm'} and benign_nan:
        return 'NAN', diffs
    if kinds == {'mem'} and view in ('f32', 'f64'):
        a, b = diffs[0][2], diffs[0][3]
        if view == 'f32':
            pairs = zip(struct.unpack('<8I', a), struct.unpack('<8I', b))
            if all(x == y or (isnan32(x) and isnan32(y)) for x, y in pairs):
                return 'NAN', diffs
    return 'BUG', diffs


def regs_of(ins):
    """Registers the instruction reads, for the input-state dump."""
    g, x, m = set(), set(), set()
    for op in ins.operands:
        if op.type == X86_OP_REG:
            n = md.reg_name(op.reg)
            if n.startswith('xmm'):
                x.add(int(n[3:]))
            elif re.fullmatch(r'mm[0-7]', n):
                m.add(int(n[2:]))
            else:
                g.add(n)
        elif op.type == X86_OP_MEM:
            if op.mem.base:
                g.add(md.reg_name(op.mem.base))
            if op.mem.index:
                g.add(md.reg_name(op.mem.index))
    base = ins.mnemonic.split()[-1]
    if base in ('mul', 'imul', 'div', 'idiv', 'cqo', 'cdq', 'cdqe', 'cwde', 'cbw', 'cwd',
                'cmpxchg', 'lahf', 'sahf', 'cmpxchg16b', 'cmpxchg8b'):
        g |= {'rax', 'rdx'}
    if base in ('cmpxchg16b', 'cmpxchg8b'):
        g |= {'rbx', 'rcx'}
    return g, x, m


PARENT = {}
for _i, _n in enumerate(GN):
    PARENT[_n] = _i
for _n, _p in (('eax', 0), ('ecx', 1), ('edx', 2), ('ebx', 3), ('ebp', 5), ('esi', 6), ('edi', 7),
               ('ax', 0), ('cx', 1), ('dx', 2), ('bx', 3), ('bp', 5), ('si', 6), ('di', 7),
               ('al', 0), ('cl', 1), ('dl', 2), ('bl', 3), ('ah', 0), ('ch', 1), ('dh', 2), ('bh', 3),
               ('bpl', 5), ('sil', 6), ('dil', 7)):
    PARENT[_n] = _p
for _i in range(8, 16):
    for _s in ('', 'd', 'w', 'b'):
        PARENT['r%d%s' % (_i, _s)] = _i


def dump_case(case, rec, cls, diffs, indent='    '):
    ins = decode(case['enc'])
    inp = rec['inp']
    L = []
    g, x, m = regs_of(ins)
    view = lane_view(ins.mnemonic)
    gi = sorted(set(PARENT[n] for n in g if n in PARENT) | set(
        GN.index(d[1]) for d in diffs if d[0] == 'gpr'))
    L.append(indent + 'input  (trial %d): flags=%s' % (rec['trial'], flag_str(inp['flags'])))
    for k in gi:
        L.append(indent + '  %-5s = %016x' % (GN[k], inp['gpr'][k]))
    xi = sorted(x | set(int(d[1][3:]) for d in diffs if d[0] == 'xmm'))
    for k in xi:
        L.append(indent + '  xmm%-2d = %s' % (k, xmm_str(inp['xmm'][k], view)))
    for k in sorted(m | set(int(d[1][2:]) for d in diffs if d[0] == 'mm')):
        L.append(indent + '  mm%d   = %016x' % (k, inp['mm'][k]))
    for d in diffs:
        kind, name, a, b = d
        if kind == 'gpr':
            L.append(indent + '%-6s native %016x   lifted %016x' % (name, a, b))
        elif kind == 'flag':
            L.append(indent + '%-6s native %d   lifted %d' % (name, a, b))
        elif kind == 'xmm':
            L.append(indent + '%-6s native %s' % (name, xmm_str(a, view)))
            L.append(indent + '%-6s lifted %s' % ('', xmm_str(b, view)))
        elif kind == 'mm':
            L.append(indent + '%-6s native %016x   lifted %016x' % (name, a, b))
        elif kind == 'mem':
            L.append(indent + name)
            L.append(indent + '  native %s' % a.hex(' '))
            L.append(indent + '  lifted %s' % b.hex(' '))
        elif kind == 'lifted-fault':
            L.append(indent + 'lifted code raised %s (SIGSEGV=11); the native instruction completed%s'
                     % (a, ' - page-edge trial: the memory operand ends exactly at a PROT_NONE page'
                        if rec['trial'] % 8 == 7 else ''))
        else:
            L.append(indent + '%s %s' % (name, a))
    return L


def origin_str(c):
    if c['origin'] != 'corpus':
        return c['origin'].replace('synth:', 'synthesised ').replace('<-', ' from ')
    if c['count'] == 0:
        return 'dt_suite (targeted)'
    pe = 'GameAssembly' if '_ga' in c.get('_run', '') else 'UnityPlayer'
    return '%s x%d, e.g. %#x' % (pe, c['count'], c['addr'])


def lifted_stmt(case):
    for line in case['body'].splitlines():
        s = line.strip()
        if s.startswith('c->rip') or s.startswith('void') or s in ('{', '}') or s.startswith('/*') \
                or s.startswith('dispatch'):
            continue
        return re.sub(r'\s*/\* 0001400000.*\*/\s*$', '', s)
    return ''


def analyze_runs(dirs):
    runs = []
    for od in dirs:
        res = json.load(open(os.path.join(od, 'results.json')))
        recs = read_records(os.path.join(od, 'dtrecords.bin'))
        runs.append((od, res, recs))
    per_mn = defaultdict(Counter)
    failing = defaultdict(list)        # mnemonic -> [(case, rec, diffs, ntrials_bug)]
    benign = defaultdict(list)
    todo = Counter()
    todo_ex = {}
    total = Counter()
    for od, res, recs in runs:
        for c in res['cases']:
            mn = c['mn'] + (' [mmx]' if re.search(r'\bmm[0-7]\b', c['ops']) else '')
            st = c['status']
            if st.startswith('skip:todo'):
                per_mn[mn]['todo'] += 1
                todo[mn] += 1
                todo_ex.setdefault(mn, '%s %s  [%s]  %s' % (c['mn'], c['ops'], c['enc'], st[10:]))
                total['todo'] += 1
                continue
            if st != 'tested':
                per_mn[mn]['other'] += 1
                total['other'] += 1
                continue
            per_mn[mn]['tested'] += 1
            total['tested'] += 1
            trials_run = c['pass'] + c['fail'] + c['lfault']
            if trials_run == 0:
                per_mn[mn]['nfault'] += 1
                total['nfault'] += 1
                continue
            cls = Counter()
            first = {}
            for r in recs.get(c['idx'], []):
                k, diffs = classify(c, r)
                cls[k] += 1
                if k not in first:
                    first[k] = (r, diffs)
            c['_cls'] = dict(cls)
            c['_trials'] = trials_run
            c['_run'] = od
            if cls['BUG']:
                per_mn[mn]['fail'] += 1
                total['fail'] += 1
                failing[mn].append((c, first['BUG'][0], first['BUG'][1], cls['BUG']))
            else:
                per_mn[mn]['pass'] += 1
                total['pass'] += 1
                for k in ('UNDEF', 'NAN'):
                    if cls[k]:
                        per_mn[mn]['pass_' + k.lower()] += 1
                        benign[(mn, k)].append((c, first[k][0], first[k][1], cls[k]))
    return dict(runs=runs, per_mn=per_mn, failing=failing, benign=benign, todo=todo,
                todo_ex=todo_ex, total=total)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdirs', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--json', default=None)
    ap.add_argument('--diag', default=os.path.join(HERE, 'diagnoses.json'))
    ap.add_argument('--compare', nargs='*', default=None, help='outdirs of a second lifter snapshot')
    ap.add_argument('--label', default='')
    ap.add_argument('--compare-label', default='')
    a = ap.parse_args()

    diag = json.load(open(a.diag)) if os.path.exists(a.diag) else []
    A = analyze_runs(a.outdirs)
    runs, per_mn, failing, benign, todo, todo_ex, total = (A[k] for k in (
        'runs', 'per_mn', 'failing', 'benign', 'todo', 'todo_ex', 'total'))
    C = analyze_runs(a.compare) if a.compare else None

    def diag_for(mn):
        for d in diag:
            if re.fullmatch(d['match'] + r'( \[mmx\])?', mn):
                return d
        return None

    def snapdesc(res):
        ld = os.path.relpath(res.get('lift_dir', '?'), ROOT) if res.get('lift_dir', '/').startswith('/') else '?'
        ri = os.path.relpath(res.get('runtime_inc', '?'), ROOT) if res.get('runtime_inc', '/').startswith('/') else '?'
        return ld.replace('/w/', ''), ri.replace('/w/', '')

    O = []
    O.append('difftest64 (Linux port) - lift64_cpu.py vs the real CPU')
    O.append('=' * 78)
    O.append('')
    O.append('tested lifter:  %s/lift64_cpu.py    runtime header: %s/cpu64.h' % snapdesc(runs[0][1]))
    if a.label:
        O.append('                (%s)' % a.label)
    if C:
        O.append('compared with:  %s/lift64_cpu.py    runtime header: %s/cpu64.h' % snapdesc(C['runs'][0][1]))
        if a.compare_label:
            O.append('                (%s)' % a.compare_label)
    for od, res, recs in runs:
        O.append('run: %-28s corpus: %-26s lifted C %s, %d trials/case'
                 % (os.path.relpath(od, ROOT), os.path.basename(res['pe']) if res['pe'] != 'dt_suite' else
                    'dt_suite.py (targeted)', res['opt'], res['ntrials']))
    O.append('host CPU: AMD Ryzen 7 8745HS (Zen 4, x86-64).  Lifted C: clang 14 -O1 -fwrapv '
             '-fno-strict-aliasing -ffp-contract=off -mcx16 (the project flags for gen/).')
    O.append('')
    O.append('cases: %d tested (%d pass, %d FAIL, %d native-faulted on every trial), '
             '%d not lifted (RECOMP_TODO), %d other skips'
             % (total['tested'], total['pass'], total['fail'], total['nfault'], total['todo'], total['other']))
    nbugmn = len(failing)
    O.append('mnemonics with confirmed mismatches: %d   (%s)' % (nbugmn, ', '.join(sorted(failing))))
    if C:
        O.append('in the compared tree: %d tested, %d FAIL; still failing: %s'
                 % (C['total']['tested'], C['total']['fail'], ', '.join(sorted(C['failing'])) or 'none'))
        new_fail = sorted(set(C['failing']) - set(failing))
        if new_fail:
            O.append('NEW failures in the compared tree (regressions): %s' % ', '.join(new_fail))
    O.append('')

    # ---- bugs ----
    O.append('1. CONFIRMED LIFTER BUGS (defined architectural state differs)')
    O.append('-' * 78)
    groups = defaultdict(list)
    for mn in failing:
        d = diag_for(mn)
        groups[d['id'] if d else 'zz:' + mn].append(mn)
    order = sorted(groups, key=lambda g: (next((i for i, d in enumerate(diag) if d['id'] == g), 999), g))
    n = 0
    for g in order:
        mns = sorted(groups[g])
        d = next((x for x in diag if x['id'] == g), None)
        n += 1
        title = d['title'] if d else 'UNDIAGNOSED: ' + ', '.join(mns)
        O.append('')
        O.append('%d.%d  %s' % (1, n, title))
        if d:
            O.append('     severity: %s' % d.get('severity', '?'))
            O.append('     affected: %s' % d.get('forms', ', '.join(mns)))
            O.append('     lifter:   %s' % d.get('where', '?'))
            for para in d.get('diagnosis', '').split('\n'):
                O.append('     ' + para)
        for mn in mns:
            lst = failing[mn]
            tot = per_mn[mn]['tested']
            O.append('')
            st = ''
            if C:
                cf, ct = len(C['failing'].get(mn, [])), C['per_mn'][mn]['tested']
                st = ('   -> compared tree: FIXED (0 of %d fail)' % ct) if not cf else \
                     ('   -> compared tree: STILL FAILS (%d of %d)' % (cf, ct))
            O.append('   %s: %d of %d tested encodings fail%s' % (mn, len(lst), tot, st))
            shown = 0
            # show distinct shapes first (reg/mem/rip/inplace)
            seen_shape = set()
            for c, r, diffs, nb in sorted(lst, key=lambda t: (-(t[0]['count'] > 0), -t[0]['count'], -t[3])):
                sh = c['info']['shape']
                if sh in seen_shape and shown >= 2:
                    continue
                if shown >= 4:
                    break
                seen_shape.add(sh)
                shown += 1
                O.append('   - %s %s   bytes %s%s   (%d/%d trials wrong; %s)'
                         % (c['mn'], c['ops'], c['enc'],
                            ('  [patched to %s]' % c['lenc']) if c['lenc'] != c['enc'] else '',
                            nb, c['_trials'], origin_str(c)))
                O.append('     lifted: %s' % lifted_stmt(c))
                O += dump_case(c, r, 'BUG', diffs, indent='       ')
            rest = [c for c, _r, _d, _n in lst]
            if len(rest) > shown:
                O.append('     other failing encodings: ' + '; '.join(
                    '%s %s' % (c['mn'], c['ops']) for c in rest[shown:shown + 12])
                         + (' ...' if len(rest) > shown + 12 else ''))
    O.append('')

    # ---- benign ----
    O.append('')
    O.append('2. DIVERGENCES THAT ARE NOT LIFTER BUGS (reported for completeness)')
    O.append('-' * 78)
    O.append('UNDEF = only flags the ISA leaves undefined for that input differ (OF after a multi-bit')
    O.append('        shift/rotate, CF after shl/shr by >= the operand width).')
    O.append('NAN   = only lanes where BOTH results are NaN differ (which input NaN propagates).')
    for (mn, k), lst in sorted(benign.items()):
        c, r, diffs, nb = lst[0]
        O.append('')
        O.append('  %-10s %-5s %d encodings, e.g. %s %s [%s] (%d/%d trials; %s)'
                 % (mn, k, len(lst), c['mn'], c['ops'], c['enc'], nb, c['_trials'], origin_str(c)))
        O += dump_case(c, r, k, diffs, indent='      ')
    O.append('')

    # ---- per mnemonic ----
    O.append('')
    O.append('3. PER-MNEMONIC COUNTS (distinct encodings; each run %d trials)' % runs[0][1]['ntrials'])
    O.append('-' * 78)
    O.append('tested/pass/FAIL: encodings run against the tested lifter; FAIL-now: still failing in the')
    O.append('compared tree; nfault: native faulted on every trial (untestable); todo: lifted as RECOMP_TODO.')
    O.append('')
    O.append('%-18s %6s %6s %6s %8s %6s %6s  %s' % ('mnemonic', 'tested', 'pass', 'FAIL', 'FAIL-now', 'nfault',
                                                  'todo', 'notes'))
    for mn in sorted(per_mn, key=lambda m: (m.replace(' [mmx]', '').split()[-1], m)):
        c = per_mn[mn]
        notes = []
        if c['pass_undef']:
            notes.append('%d pass w/ undef-flag diffs' % c['pass_undef'])
        if c['pass_nan']:
            notes.append('%d pass w/ NaN-payload diffs' % c['pass_nan'])
        now = ''
        if C and c['tested']:
            now = len(C['failing'].get(mn, [])) or ('0' if c['fail'] else '')
        O.append('%-18s %6d %6d %6s %8s %6s %6s  %s' % (mn, c['tested'], c['pass'], c['fail'] or '', now,
                                                      c['nfault'] or '', c['todo'] or '', '; '.join(notes)))
    O.append('')
    O.append('')
    O.append('4. NOT LIFTED (RECOMP_TODO) - excluded from the comparison, listed so nothing is silent')
    O.append('-' * 78)
    for mn, k in sorted(todo.items(), key=lambda t: -t[1]):
        O.append('  %-14s %3d   e.g. %s' % (mn, k, todo_ex[mn]))
    O.append('')
    O.append('')
    O += open(os.path.join(HERE, 'report_method.txt')).read().rstrip().splitlines()
    with open(a.out, 'w') as f:
        f.write('\n'.join(O) + '\n')
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({mn: [dict(mn=c['mn'], ops=c['ops'], enc=c['enc'], cls=c['_cls'])
                            for c, _r, _d, _n in lst] for mn, lst in failing.items()}, f, indent=1)
    print('[report] %d tested, %d pass, %d FAIL across %d mnemonics -> %s'
          % (total['tested'], total['pass'], total['fail'], nbugmn, a.out))


if __name__ == '__main__':
    main()
