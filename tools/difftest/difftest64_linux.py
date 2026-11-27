#!/usr/bin/env python3
"""
difftest64_linux.py - Linux port of pcrecomp's difftest64 (lift64_cpu vs the real CPU).

Same method as tools/lift/difftest64.py: execute the ORIGINAL instruction bytes
natively from a known register file (dt_native.S, GAS instead of MASM) and the
lifted C from the same state, then compare GPRs (not RSP), the modelled flags,
xmm0-15 and - new here - mm0-7 and memory. Reused from the original: the skip
list, the register/scratch-buffer classification of memory operands, the GPR
edge values, the undefined-flag masks and the 1.5*2^-12 rule for rcp/rsqrt.

What is different:
  * SEH -> sigsetjmp/siglongjmp (dt_driver.c); MASM -> GAS; cmd/cl -> clang.
  * The lifted half is built against the PROJECT runtime header
    (runtime/include/cpu64.h: mm[8] after mxcsr, always_inline helpers,
    w64_todo, w64_teb) with the project's flags for generated code
    (-O1 -fwrapv -fno-strict-aliasing -ffp-contract=off -mcx16).
  * Corpus: every distinct operand FORM of the binary (dt_corpus.py), then a
    stratified per-mnemonic selection (operand shape, in-place dst==src,
    immediates), plus SYNTHESISED variants: dst==src in-place forms and
    register forms of memory-only encodings (ModRM rewritten, re-decoded by
    capstone, kept only if they still decode to a lifter-known instruction).
  * RIP-relative operands are tested by patching the disp32 so the operand
    lands in the scratch buffer (fixed code->scratch offset, MAP_32BIT), and
    running the lifted code with g_image_delta = page - 0x140000000.
  * Seeds add float/double lane edge values (signed zeros, denormals, inf,
    qNaN/sNaN, rounding ties, int-conversion limits), saturation-edge integer
    lanes and small shift counts; 64 trials per case.
  * div/idiv are tested (dividend high half constructed to fit), rep string
    ops get a small RCX, bt-family memory forms get a small signed bit offset.
  * The C side records raw mismatches; classification (undefined flags by
    shift count, NaN-payload-only differences) is done in dt_report.py.

Usage:
  difftest64_linux.py <corpus.json> <pe> <outdir> [--k-sse N] [--k-int N]
                      [--exclude corpus2.json] [--only REGEX] [--opt -O1]
"""
import argparse
import json
import os
import random
import re
import struct
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
# DT_LIFT_DIR / DT_INC select a frozen snapshot of the lifter and the runtime header
# (work/difftest/snap_*), so a result stays reproducible while the live tree changes.
LIFT_DIR = os.path.abspath(os.environ.get('DT_LIFT_DIR') or
                           os.path.join(ROOT, 'third_party', 'pcrecomp', 'tools', 'lift'))
RUNTIME_INC = os.path.abspath(os.environ.get('DT_INC') or os.path.join(ROOT, 'runtime', 'include'))
sys.path.insert(0, LIFT_DIR)

import lift64_cpu                                        # noqa: E402
from lift64_cpu import Lifter                            # noqa: E402
import difftest64 as dt0                                 # noqa: E402  (constants reused)
from capstone import Cs, CS_ARCH_X86, CS_MODE_64         # noqa: E402
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM, X86_REG_RIP  # noqa: E402

LIFT_VA = 0x140000000
RIP_TARGET_OFF = 0x3000           # scratch midpoint - code page (dt_driver.c layout)
NSEEDS, NTRIALS, NSTRUCT = 256, 64, 16

CF, PF, AF, ZF, SF, OF = 1, 4, 16, 64, 128, 2048
ALLF = CF | PF | AF | ZF | SF | OF

SKIP = set(dt0.SKIP_MNEMONICS) - {'div', 'idiv'}
SKIP |= {
    'ins', 'insb', 'insw', 'insd', 'outs', 'outsb', 'outsw', 'outsd', 'retf', 'iretd',
    'int1', 'into', 'clts', 'invd', 'wbinvd', 'lfs', 'lgs', 'lss', 'rdsspq', 'rdsspd',
    'xgetbv', 'xsetbv', 'rdtscp', 'rdrand', 'rdseed', 'rdpid', 'rdpmc', 'rdmsr', 'wrmsr',
    'swapgs', 'rdfsbase', 'rdgsbase', 'wrfsbase', 'wrgsbase', 'xbegin', 'xend', 'xabort',
    'xtest', 'std', 'clac', 'stac', 'ud0', 'ud1', 'ud2', 'monitor', 'mwait', 'fxsave',
    'fxrstor', 'fxsave64', 'fxrstor64', 'xsave', 'xrstor', 'xsave64', 'xrstor64',
    'xsaveopt', 'xsavec', 'clflush', 'clflushopt', 'clwb', 'femms', 'lgdt', 'sgdt',
    'lidt', 'sidt', 'lmsw', 'smsw', 'invlpg', 'sysenter', 'sysexit', 'vmcall',
    'prefetchnta', 'prefetcht2', 'prefetch', 'endbr32', 'lsl', 'lar', 'arpl', 'bound',
}
NOOP_OK = {'cdqe', 'cqo', 'cdq', 'cwde', 'cbw', 'cwd', 'clc', 'stc', 'cmc', 'sahf', 'lahf',
           'cld', 'emms', 'lfence', 'mfence', 'sfence', 'endbr64'}

# AF is compared except where the ISA leaves it undefined (the original masked AF
# for every instruction; it is DEFINED for add/sub/inc/dec/neg/cmp and preserved
# by everything that does not write flags).
AF_UNDEF = {'and', 'or', 'xor', 'test', 'shl', 'sal', 'shr', 'sar', 'shld', 'shrd',
            'mul', 'imul', 'bt', 'bts', 'btr', 'btc', 'bsf', 'bsr', 'tzcnt', 'lzcnt',
            'div', 'idiv', 'andn', 'bextr', 'blsi', 'blsr', 'blsmsk'}
EXTRA_UNDEF = {
    'tzcnt': OF | SF | PF | AF, 'lzcnt': OF | SF | PF | AF,
    'div': ALLF, 'idiv': ALLF,
}


def undef_mask(base):
    u = dt0.UNDEFINED_FLAGS.get(base, 0) & ~AF
    if base in AF_UNDEF:
        u |= AF
    u |= EXTRA_UNDEF.get(base, 0)
    return u


GPR_EDGE = list(dt0.EDGE_VALUES) + [
    0x8, 0x10, 0x1F, 0x20, 0x3F, 0x40, 0x7F, 0x8000000000000001, 0xFFFFFFFF80000000,
    0x00000000FFFF0000, 0xFFFFFFFFFFFFFF80, 0x0000000100000000, 0x4142434445464748,
]


def f32b(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def f64b(x):
    return struct.unpack('<Q', struct.pack('<d', x))[0]


F32_EDGE = [
    0x00000000, 0x80000000, f32b(1), f32b(-1), f32b(.5), f32b(-.5), f32b(1.5), f32b(2.5),
    f32b(-2.5), f32b(3.5), f32b(-3.5), 0x3EFFFFFF, 0x00000001, 0x007FFFFF, 0x00800000,
    0x80000001, 0x807FFFFF, 0x7F7FFFFF, 0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000,
    0xFFC00000, 0x7F800001, 0x7FA00000, 0x7FC12345, 0xFFA00001, 0x4F000000, 0xCF000000,
    0x4EFFFFFF, 0xCF000001, 0x4F800000, 0x5F000000, 0xDF000000, 0x5EFFFFFF, f32b(65535),
    f32b(32767.5), f32b(-32768.5), f32b(255.5), f32b(.75), f32b(3), f32b(100.25),
    f32b(-7.5), f32b(1e10), f32b(16777216), 0x4B800001, f32b(1e-30), f32b(-1e-38),
    f32b(2), f32b(4), f32b(0.25), f32b(-0.0001),
]
F64_EDGE = [
    0, 1 << 63, f64b(1), f64b(-1), f64b(.5), f64b(-.5), f64b(1.5), f64b(2.5), f64b(-2.5),
    f64b(3.5), 1, 0x000FFFFFFFFFFFFF, 0x0010000000000000, 0x8000000000000001,
    0x7FEFFFFFFFFFFFFF, 0xFFEFFFFFFFFFFFFF, 0x7FF0000000000000, 0xFFF0000000000000,
    0x7FF8000000000000, 0xFFF8000000000000, 0x7FF0000000000001, 0x7FF4000000000000,
    0x7FF8000012345678, 0xFFF4000000000001, f64b(2147483647.5), f64b(2147483648.0),
    f64b(-2147483648.5), f64b(-2147483649.0), f64b(-2147483648.0), f64b(2147483647.0),
    f64b(9223372036854775808.0), f64b(-9223372036854775808.0), f64b(9223372036854774784.0),
    f64b(4294967295.5), f64b(1e300), f64b(1e-300), f64b(3.4028234663852886e38),
    f64b(3.4028235677973366e38), f64b(1.401298464324817e-45), f64b(1e-46), f64b(16777217.0),
    f64b(0.1), f64b(65535.5), f64b(-32768.5), f64b(1.0000000596046448),
]
I8_EDGE = [0, 1, 0x7F, 0x80, 0xFF, 0xFE, 0x81, 0x40]
I16_EDGE = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x7FFE, 0x8001, 0x00FF, 0x0100, 0xFF00, 0x0080,
            0x007F, 0xFF80, 0xFF7F]
I32_EDGE = [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x0000FFFF, 0x00008000, 0x00007FFF,
            0xFFFF8000, 0xFFFF7FFF, 0x00010000, 0x7FFF8000]
COUNTS = [0, 1, 2, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 255,
          1 << 32, 1 << 63]


def nice_f32(rng):
    r = rng.random()
    if r < 0.25:
        return f32b(float(rng.randint(-70000, 70000)) + rng.choice((0, 0, 0.5, 0.25)))
    s = -1.0 if rng.random() < 0.4 else 1.0
    return f32b(s * (1 + rng.random()) * 2.0 ** rng.randint(-12, 26))


def nice_f64(rng):
    r = rng.random()
    if r < 0.25:
        return f64b(float(rng.randint(-3000000000, 3000000000)) + rng.choice((0, 0, 0.5, 0.25)))
    s = -1.0 if rng.random() < 0.4 else 1.0
    return f64b(s * (1 + rng.random()) * 2.0 ** rng.randint(-30, 66))


def lanes_to_u64(vals, bits):
    v = 0
    for i, x in enumerate(vals):
        v |= (x & ((1 << bits) - 1)) << (i * bits)
    return v & ((1 << 128) - 1)


def gen_xmm(rng, mode):
    if mode == 'f32':
        lanes = [rng.choice(F32_EDGE) if rng.random() < 0.45 else
                 (nice_f32(rng) if rng.random() < 0.85 else rng.getrandbits(32)) for _ in range(4)]
        return lanes_to_u64(lanes, 32)
    if mode == 'f64':
        lanes = [rng.choice(F64_EDGE) if rng.random() < 0.45 else
                 (nice_f64(rng) if rng.random() < 0.85 else rng.getrandbits(64)) for _ in range(2)]
        return lanes_to_u64(lanes, 64)
    if mode in ('i8', 'i16', 'i32'):
        bits, tbl = {'i8': (8, I8_EDGE), 'i16': (16, I16_EDGE), 'i32': (32, I32_EDGE)}[mode]
        lanes = [rng.choice(tbl) if rng.random() < 0.5 else rng.getrandbits(bits)
                 for _ in range(128 // bits)]
        return lanes_to_u64(lanes, bits)
    if mode == 'cnt':
        return rng.choice(COUNTS) | (rng.getrandbits(64) << 64)
    if rng.random() < 0.3:
        return rng.choice(GPR_EDGE) | (rng.choice(GPR_EDGE) << 64)
    return rng.getrandbits(128)


XMODES = ['f32'] * 30 + ['f64'] * 15 + ['i8'] * 10 + ['i16'] * 12 + ['i32'] * 8 + ['cnt'] * 5 + ['raw'] * 20
MMODES = ['i8'] * 20 + ['i16'] * 25 + ['i32'] * 15 + ['cnt'] * 10 + ['raw'] * 30


def make_seeds(rng):
    seeds = []
    for i in range(NSEEDS):
        gpr = []
        for k in range(16):
            if i < len(GPR_EDGE):
                gpr.append(GPR_EDGE[(i + k) % len(GPR_EDGE)])
            elif rng.random() < 0.35:
                gpr.append(rng.choice(GPR_EDGE))
            elif rng.random() < 0.1:
                gpr.append(rng.randint(0, 70))
            else:
                gpr.append(rng.getrandbits(64))
        flags = rng.getrandbits(16) & ALLF
        xmm = []
        for k in range(16):
            if i < NSTRUCT:
                # structured: whole register file in one lane type, edge values
                # spread so that register pairs meet NaN/inf/zero combinations
                if i % 2 == 0:
                    lanes = [F32_EDGE[(i * 5 + k * 4 + l * 11) % len(F32_EDGE)] for l in range(4)]
                    xmm.append(lanes_to_u64(lanes, 32))
                else:
                    lanes = [F64_EDGE[(i * 3 + k * 2 + l * 7) % len(F64_EDGE)] for l in range(2)]
                    xmm.append(lanes_to_u64(lanes, 64))
            else:
                xmm.append(gen_xmm(rng, rng.choice(XMODES)))
        mm = []
        for k in range(8):
            v = gen_xmm(rng, rng.choice(MMODES))
            mm.append(v & ((1 << 64) - 1))
        seeds.append((gpr, flags, xmm, mm))
    return seeds


def emit_seeds(path, seeds):
    L = ['/* generated by difftest64_linux.py */',
         '#ifndef DTSEEDS_H', '#define DTSEEDS_H',
         'typedef struct { uint64_t gpr[16]; uint64_t flags; uint64_t _pad;',
         '                 XMM xmm[16]; uint64_t mm[8]; } DTSTATE;',
         '_Static_assert(sizeof(DTSTATE) == 464, "DTSTATE layout is hard-coded in dt_native.S");',
         '#define NSEEDS %d' % NSEEDS, '#define NTRIALS %d' % NTRIALS, '#define NSTRUCT %d' % NSTRUCT,
         'static const DTSTATE seeds[NSEEDS] __attribute__((aligned(64))) = {']
    for gpr, flags, xmm, mm in seeds:
        g = ','.join('0x%XULL' % v for v in gpr)
        x = ','.join('{.u64={0x%XULL,0x%XULL}}' % (v & ((1 << 64) - 1), v >> 64) for v in xmm)
        m = ','.join('0x%XULL' % v for v in mm)
        L.append('  { {%s}, 0x%XULL, 0, {%s}, {%s} },' % (g, flags, x, m))
    L += ['};', '#endif']
    with open(path, 'w') as f:
        f.write('\n'.join(L) + '\n')


# ---------------------------------------------------------------- corpus --

GPR8H = {'ah', 'bh', 'ch', 'dh'}


def regkind(n):
    if re.fullmatch(r'xmm\d+', n):
        return 'x'
    if re.fullmatch(r'mm[0-7]', n):
        return 'mm'
    if n in lift64_cpu.R64:
        return 'r64'
    if n in lift64_cpu.R32:
        return 'r32'
    if n in lift64_cpu.R16:
        return 'r16'
    if n in GPR8H:
        return 'r8h'
    if n in lift64_cpu.R8L:
        return 'r8'
    return None


def base_mn(m):
    p = m.split()
    return p[-1], (p[0] if len(p) > 1 else '')


def analyze(md, insn):
    """None if untestable, else the aiming info and an operand-shape key."""
    m = insn.mnemonic
    base, pfx = base_mn(m)
    if pfx in ('bnd', 'notrack'):
        return None
    if base in SKIP or m in SKIP or base.startswith('prefetch'):
        return None
    if base.startswith('j') or base.startswith('loop') or base[0] == 'f':
        return None
    if base.startswith('v') and base not in ('verr', 'verw'):
        return None                                  # AVX: not lifted
    ops = insn.operands
    if not ops and base not in NOOP_OK:
        return None
    info = dict(basem=0, idxm=0, smallm=0, bitoffm=0, dsp=0, rip=False, scale=1, div=0, msize=0)
    kinds = []
    regs = []
    lea = base == 'lea'
    for op in ops:
        if op.type == X86_OP_REG:
            n = md.reg_name(op.reg)
            if n in dt0.BAD_REGS:
                return None
            k = regkind(n)
            if k is None:
                return None
            kinds.append(k)
            regs.append(n)
        elif op.type == X86_OP_IMM:
            kinds.append('i%d' % op.size)
        elif op.type == X86_OP_MEM:
            mem = op.mem
            if mem.segment:
                return None
            if mem.base == X86_REG_RIP:
                if mem.index or insn.disp_size != 4:
                    return None
                info['rip'] = True
                kinds.append('m%d.rip' % op.size)
                continue
            if not mem.base:
                return None
            if abs(mem.disp) >= 0x800:
                return None
            bn = md.reg_name(mem.base)
            bi = dt0.reg_index(bn) if bn else -1
            if bi < 0 or bn in dt0.BAD_REGS:
                return None
            if not lea:
                info['basem'] |= 1 << bi
            info['dsp'] = mem.disp
            if mem.index:
                inm = md.reg_name(mem.index)
                ii = dt0.reg_index(inm) if inm else -1
                if ii < 0 or inm in dt0.BAD_REGS:
                    return None
                if not lea:
                    info['idxm'] |= 1 << ii
                    info['scale'] = mem.scale
            kinds.append('m%d.%s' % (op.size, 'bi' if mem.index else 'b'))
            if not lea:
                info['msize'] = max(info['msize'], op.size)
        else:
            return None
    if info['basem'] & info['idxm']:
        return None
    if pfx in ('rep', 'repe', 'repz', 'repne', 'repnz') and any(k.startswith('m') for k in kinds):
        info['smallm'] |= 1 << 1                     # rcx
    if base in ('bt', 'bts', 'btr', 'btc') and ops[0].type == X86_OP_MEM and ops[1].type == X86_OP_REG:
        ri = dt0.reg_index(md.reg_name(ops[1].reg))
        if ri < 0 or (info['basem'] | info['idxm']) & (1 << ri):
            return None
        info['bitoffm'] |= 1 << ri
    if info['smallm'] or info['bitoffm'] or base.startswith('prefetch'):
        info['msize'] = 0                            # no page-edge trial: the access is not one operand
    if base in ('div', 'idiv'):
        info['div'] = ops[0].size | (0x10 if base == 'idiv' else 0)
        # the divisor must not be the dividend's own registers' aiming target
    inplace = len(regs) >= 2 and len(set(regs[:2])) == 1
    imm = ''
    if kinds and kinds[-1].startswith('i') and (insn.imm_size == 1 or base[0] in 'ps' or 'sh' in base or 'ro' in base):
        imm = '#%x' % (ops[-1].imm & 0xFF)
    shape = '%s %s%s%s' % (pfx, ','.join(kinds), ' inplace' if inplace else '', imm)
    info['shape'] = shape.strip()
    info['inplace'] = inplace
    info['simd'] = any(k in ('x', 'mm') for k in kinds) or base in lift64_cpu.SSE64_MNEMONICS
    return info


LEGACY = {0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65, 0x66, 0x67}


def synth_variants(b, insn):
    """dst==src and register forms of an encoding, by rewriting the ModRM."""
    mo = insn.modrm_offset
    if mo <= 0 or mo >= len(b):
        return []
    i = 0
    while i < len(b) and b[i] in LEGACY:
        i += 1
    rexpos = i if i < len(b) and 0x40 <= b[i] <= 0x4F else -1
    modrm = b[mo]
    mod, reg, rm = modrm >> 6, (modrm >> 3) & 7, modrm & 7
    rex = b[rexpos] if rexpos >= 0 else 0
    R, B = (rex >> 2) & 1, rex & 1
    imm = b[insn.imm_offset: insn.imm_offset + insn.imm_size] if insn.imm_size else b''
    head = bytearray(b[:mo])

    def mk(newrm, newB):
        h = bytearray(head)
        if rexpos >= 0:
            h[rexpos] = (h[rexpos] & ~0x3) | newB
        elif newB:
            return None
        return bytes(h) + bytes([0xC0 | (reg << 3) | newrm]) + bytes(imm)

    out = []
    if mod == 3:
        if (rm, B) != (reg, R):
            out.append(('inplace', mk(reg, R)))
    else:
        out.append(('regform', mk((reg + 1) & 7, R)))
        out.append(('regform-inplace', mk(reg, R)))
    return [(t, v) for t, v in out if v]


def decode1(md, b):
    ins = list(md.disasm(b, LIFT_VA))
    if len(ins) != 1 or ins[0].size != len(b):
        return None
    return ins[0]


def select_cases(md, forms, k_sse, k_int, only=None, exclude=None):
    """Stratified per-mnemonic selection, plus synthesised variants."""
    bym = defaultdict(list)
    for key, (cnt, enc, addr) in forms.items():
        if exclude and key in exclude:
            continue
        mn = key.split(' ')[0]
        if only and not re.search(only, key):
            continue
        bym[mn].append((cnt, key, enc, addr))
    known_mn = set(k.split(' ')[0] for k in forms)
    cases = []
    seen = set()
    for mn_first, lst in sorted(bym.items()):
        decoded = []
        for cnt, key, enc, addr in lst:
            b = bytes.fromhex(enc)
            ins = decode1(md, b)
            if ins is None:
                continue
            info = analyze(md, ins)
            if info is None:
                continue
            decoded.append((cnt, key, b, addr, ins, info))
        if not decoded:
            continue
        # group by base mnemonic (lock/rep variants count as their own group)
        groups = defaultdict(list)
        for d in decoded:
            groups[d[4].mnemonic].append(d)
        for m, lst2 in groups.items():
            simd = any(d[5]['simd'] for d in lst2)
            K = k_sse if simd else k_int
            if m in ('mov', 'lea', 'movzx', 'movsx', 'movsxd', 'add', 'sub', 'cmp', 'and', 'or',
                     'xor', 'test', 'imul', 'shl', 'shr', 'sar', 'rol', 'ror'):
                K = max(K, 32)
            byshape = defaultdict(list)
            for d in lst2:
                byshape[d[5]['shape']].append(d)
            for s in byshape:
                byshape[s].sort(key=lambda d: -d[0])
            order = sorted(byshape, key=lambda s: (-('inplace' in s), -sum(d[0] for d in byshape[s])))
            picked = []
            r = 0
            while len(picked) < K:
                added = False
                for s in order:
                    if r < len(byshape[s]) and len(picked) < K:
                        picked.append(byshape[s][r])
                        added = True
                r += 1
                if not added:
                    break
            nsyn = 0
            for cnt, key, b, addr, ins, info in picked:
                if b in seen:
                    continue
                seen.add(b)
                cases.append(dict(enc=b, mn=ins.mnemonic, ops=ins.op_str, count=cnt, addr=addr,
                                  origin='corpus', info=info))
            for cnt, key, b, addr, ins, info in picked:
                if nsyn >= (12 if simd else 4):
                    break
                if base_mn(ins.mnemonic)[1]:             # lock/rep: register forms are #UD
                    continue
                for tag, vb in synth_variants(b, ins):
                    if vb in seen:
                        continue
                    vi = decode1(md, vb)
                    if vi is None:
                        continue
                    vinfo = analyze(md, vi)
                    if vinfo is None:
                        continue
                    if vi.mnemonic not in known_mn and base_mn(vi.mnemonic)[0] not in lift64_cpu.SSE64_MNEMONICS:
                        continue
                    seen.add(vb)
                    cases.append(dict(enc=vb, mn=vi.mnemonic, ops=vi.op_str, count=0, addr=addr,
                                      origin='synth:%s<-%s' % (tag, b.hex()), info=vinfo))
                    nsyn += 1
    return cases


# ----------------------------------------------------------------- build --

def patch_rip(md, b):
    ins = decode1(md, b)
    do, ds = ins.disp_offset, ins.disp_size
    if ds != 4:
        return None
    nb = b[:do] + struct.pack('<i', RIP_TARGET_OFF - len(b)) + b[do + 4:]
    ni = decode1(md, nb)
    if ni is None or ni.mnemonic != ins.mnemonic:
        return None
    return nb


def lift_cases(md, cases, lifter):
    out = []
    for c in cases:
        b = c['enc']
        lb = b
        if c['info']['rip']:
            lb = patch_rip(md, b)
            if lb is None:
                c['status'] = 'skip:rip-patch'
                continue
        c['lenc'] = lb
        name = 't_%05d' % len(out)
        try:
            body = lifter.lift_function(lb, LIFT_VA, name=name)
        except Exception as e:                       # noqa: BLE001
            c['status'] = 'skip:lift-error %s' % e
            continue
        if 'RECOMP_TODO' in body:
            m = re.search(r'RECOMP_TODO\([^,]+, "([^"]*)"', body)
            c['status'] = 'skip:todo %s' % (m.group(1) if m else '')
            continue
        c['status'] = 'tested'
        c['name'] = name
        c['body'] = body
        c['idx'] = len(out)
        out.append(c)
    return out


def emit_cases(outdir, tested, nchunks):
    hdr = ['/* generated by difftest64_linux.py */', '#ifndef DTCASES_H', '#define DTCASES_H',
           '#define RIP_TARGET_OFF 0x%Xu' % RIP_TARGET_OFF,
           'typedef struct { const unsigned char *code; int len; void (*fn)(CPU *);',
           '  int approx; unsigned undef, basem, idxm, smallm, bitoffm; int dadj, rip, div, scale, msize, dsp; } CASE;',
           'extern const CASE dt_cases[];', 'extern const int dt_ncases;', '#endif']
    with open(os.path.join(outdir, 'dtcases.h'), 'w') as f:
        f.write('\n'.join(hdr) + '\n')
    chunks = [[] for _ in range(nchunks)]
    for c in tested:
        chunks[c['idx'] % nchunks].append(c)
    files = []
    for k, ch in enumerate(chunks):
        p = os.path.join(outdir, 'dtlift_%02d.c' % k)
        with open(p, 'w') as f:
            f.write('/* generated by difftest64_linux.py - lifted bodies */\n#include "cpu64.h"\n\n')
            for c in ch:
                f.write('/* %s %s  [%s] */\n' % (c['mn'], c['ops'], c['enc'].hex()))
                f.write(c['body'] + '\n\n')
        files.append(p)
    L = ['/* generated by difftest64_linux.py */', '#include "cpu64.h"', '#include "dtcases.h"', '']
    for c in tested:
        L.append('void %s(CPU *c);' % c['name'])
    for c in tested:
        L.append('static const unsigned char b_%05d[] = {%s};'
                 % (c['idx'], ','.join('0x%02X' % x for x in c['lenc'])))
    L.append('const CASE dt_cases[] = {')
    for c in tested:
        info = c['info']
        base = base_mn(c['mn'])[0]
        approx = 0
        if base in dt0.APPROX_MNEMONICS:
            approx = 1 if base.endswith('ss') else 2
        L.append('  { b_%05d, %d, %s, %d, 0x%Xu, 0x%Xu, 0x%Xu, 0x%Xu, 0x%Xu, %d, %d, %d, %d, %d, %d },'
                 % (c['idx'], len(c['lenc']), c['name'], approx, undef_mask(base),
                    info['basem'], info['idxm'], info['smallm'], info['bitoffm'],
                    (-info['dsp']) & 15, 1 if info['rip'] else 0, info['div'], info['scale'],
                    info.get('msize', 0), info['dsp']))
    L.append('};')
    L.append('const int dt_ncases = %d;' % len(tested))
    p = os.path.join(outdir, 'dtcases.c')
    with open(p, 'w') as f:
        f.write('\n'.join(L) + '\n')
    files.append(p)
    return files


def build(outdir, files, opt):
    cflags = [opt, '-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off', '-mcx16', '-w',
              '-I', RUNTIME_INC, '-I', outdir]
    objs = []

    def cc(src):
        o = src[:-2] + '.o'
        r = subprocess.run(['clang'] + cflags + ['-c', src, '-o', o], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit('[!] compile failed: %s\n%s' % (src, r.stderr[-4000:]))
        return o

    srcs = files + [os.path.join(HERE, 'dt_driver.c')]
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        objs = list(ex.map(cc, srcs[:-1]))
    # the driver lives in HERE; build its object into outdir
    o = os.path.join(outdir, 'dt_driver.o')
    r = subprocess.run(['clang', '-O1', '-g', '-fwrapv', '-fno-strict-aliasing', '-w', '-I', RUNTIME_INC,
                        '-I', outdir, '-c', srcs[-1], '-o', o], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('[!] driver compile failed:\n%s' % r.stderr[-4000:])
    objs.append(o)
    exe = os.path.join(outdir, 'difftest64')
    r = subprocess.run(['clang', '-no-pie', os.path.join(HERE, 'dt_native.S')] + objs
                       + ['-o', exe, '-lm'], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit('[!] link failed:\n%s' % r.stderr[-4000:])
    return exe


def run(exe, outdir, n):
    """Run, resuming past any case that crashes the harness or hangs."""
    summary = {}
    bad = {}
    start = 0
    rec = os.path.join(outdir, 'dtrecords.bin')
    while start < n:
        r = subprocess.run([exe, str(start), rec], capture_output=True, text=True, cwd=outdir)
        last = start - 1
        for line in r.stdout.splitlines():
            p = line.split()
            if not p:
                continue
            if p[0] == 'S':
                i = int(p[1])
                summary[i] = tuple(int(x) for x in p[2:6])
                last = i
            elif p[0] in ('CRASH', 'HANG'):
                i = int(p[1])
                bad[i] = line.strip()
                last = i
        if 'DONE' in r.stdout:
            break
        if last < start:                       # no progress and no marker
            bad[start] = 'exit %d: %s' % (r.returncode, r.stderr[-300:])
            last = start
        start = last + 1
    return summary, bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('corpus')
    ap.add_argument('pe')
    ap.add_argument('outdir')
    ap.add_argument('--k-sse', type=int, default=48)
    ap.add_argument('--k-int', type=int, default=20)
    ap.add_argument('--exclude', default=None, help='corpus json whose forms are skipped')
    ap.add_argument('--only', default=None, help='regex on the form key')
    ap.add_argument('--opt', default='-O1')
    ap.add_argument('--chunks', type=int, default=32)
    a = ap.parse_args()
    a.outdir = os.path.abspath(a.outdir)
    os.makedirs(a.outdir, exist_ok=True)
    forms = json.load(open(a.corpus))['forms']
    exclude = set(json.load(open(a.exclude))['forms']) if a.exclude else None
    data = open(a.pe, 'rb').read()
    base, soi, _secs = lift64_cpu._pe_sections(data)
    lift64_cpu.IMAGE_BASE = base

    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    rng = random.Random(1234567)
    print('[*] %d forms in corpus' % len(forms))
    cases = select_cases(md, forms, a.k_sse, a.k_int, a.only, exclude)
    print('[*] %d candidate cases (%d synthesised)'
          % (len(cases), sum(1 for c in cases if c['origin'] != 'corpus')))
    lifter = Lifter(image_size=soi, read_va=None, image_base=base)
    tested = lift_cases(md, cases, lifter)
    print('[*] %d lifted cleanly, %d skipped' % (len(tested), len(cases) - len(tested)))

    emit_seeds(os.path.join(a.outdir, 'dtseeds.h'), make_seeds(rng))
    files = emit_cases(a.outdir, tested, a.chunks)
    print('[*] building (%s)' % a.opt)
    exe = build(a.outdir, files, a.opt)
    print('[*] running %d cases x %d trials' % (len(tested), NTRIALS))
    summary, bad = run(exe, a.outdir, len(tested))
    res = []
    for c in cases:
        e = dict(mn=c['mn'], ops=c['ops'], enc=c['enc'].hex(), count=c['count'], addr=c['addr'],
                 origin=c['origin'], status=c['status'], info={k: v for k, v in c['info'].items()})
        if c['status'] == 'tested':
            e['idx'] = c['idx']
            e['lenc'] = c['lenc'].hex()
            e['body'] = c['body']
            e['undef'] = undef_mask(base_mn(c['mn'])[0])
            if c['idx'] in bad:
                e['status'] = 'harness:' + bad[c['idx']]
            elif c['idx'] in summary:
                e['pass'], e['fail'], e['nfault'], e['lfault'] = summary[c['idx']]
            else:
                e['status'] = 'harness:no-result'
        res.append(e)
    with open(os.path.join(a.outdir, 'results.json'), 'w') as f:
        json.dump({'pe': a.pe, 'opt': a.opt, 'ntrials': NTRIALS, 'lift_dir': LIFT_DIR,
                   'runtime_inc': RUNTIME_INC, 'cases': res}, f)
    nf = sum(1 for e in res if e.get('fail', 0) or e.get('lfault', 0))
    print('[*] %d tested, %d with raw mismatches, %d harness problems' % (len(tested), nf, len(bad)))


if __name__ == '__main__':
    main()
