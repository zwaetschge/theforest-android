#!/usr/bin/env python3
"""
dt_suite.py - a hand-targeted instruction suite for difftest64_linux.

The corpus is weighted the way the game uses the ISA, which is the right
default and also its blind spot: it never contains `ror al, 8`, cmppd, or an
MMX shift by 64. This assembles those edge FORMS with GNU as and writes them
in dt_corpus.py's JSON shape, so the same selector/lifter/harness runs them.

Usage: dt_suite.py <out.json>   (needs `as`, `objcopy`)
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from dt_corpus import form_key                       # noqa: E402
from capstone import Cs, CS_ARCH_X86, CS_MODE_64     # noqa: E402

L = []


def add(*xs):
    L.extend(xs)


GW = {8: ['al', 'cl', 'dl', 'bh', 'sil', 'r9b'], 16: ['ax', 'cx', 'dx', 'r10w'],
      32: ['eax', 'ecx', 'edx', 'r11d'], 64: ['rax', 'rcx', 'rdx', 'r12']}
PTR = {8: 'byte', 16: 'word', 32: 'dword', 64: 'qword'}

# ---- shifts and rotates: every count class for every width ----
for op in ('rol', 'ror', 'rcl', 'rcr', 'shl', 'shr', 'sar'):
    for w, regs in GW.items():
        r = regs[0]
        for cnt in (0, 1, 2, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33, 63, 64, 65):
            add('%s %s, %d' % (op, r, cnt))
        add('%s %s, cl' % (op, r), '%s %s, 1' % (op, regs[1]), '%s %s, cl' % (op, regs[2]),
            '%s %s ptr [rbx], cl' % (op, PTR[w]), '%s %s ptr [rbx + 4], 8' % (op, PTR[w]))
    add('%s ah, cl' % op, '%s bh, 8' % op, '%s cl, cl' % op)
for op in ('shld', 'shrd'):
    for w in (16, 32, 64):
        a, b = GW[w][0], GW[w][2]
        for cnt in (0, 1, 2, 8, 15, 16, 17, 31, 32, 33, 63):
            add('%s %s, %s, %d' % (op, a, b, cnt))
        add('%s %s, %s, cl' % (op, a, b), '%s %s, %s, cl' % (op, a, a),
            '%s %s ptr [rbx], %s, cl' % (op, PTR[w], b))

# ---- bit tests: register, immediate beyond the width, and memory bit strings ----
for op in ('bt', 'bts', 'btr', 'btc'):
    for w in (16, 32, 64):
        a, b = GW[w][0], GW[w][2]
        add('%s %s, %s' % (op, a, b), '%s %s, %s' % (op, a, a))
        for imm in (0, 5, 15, 16, 31, 32, 63, 64, 100):
            add('%s %s, %d' % (op, a, imm))
            add('%s %s ptr [rbx], %d' % (op, PTR[w], imm))
        add('%s %s ptr [rbx], %s' % (op, PTR[w], b), '%s %s ptr [rbx + 8], %s' % (op, PTR[w], b))
for op in ('bsf', 'bsr', 'tzcnt', 'lzcnt', 'popcnt'):
    for w in (16, 32, 64):
        add('%s %s, %s' % (op, GW[w][0], GW[w][2]), '%s %s, %s' % (op, GW[w][0], GW[w][0]),
            '%s %s, %s ptr [rbx]' % (op, GW[w][1], PTR[w]))

# ---- multiply / divide / misc integer at every width ----
for w, regs in GW.items():
    a, b = regs[0], regs[2]
    for op in ('mul', 'imul', 'div', 'idiv', 'neg', 'not', 'inc', 'dec'):
        add('%s %s' % (op, b), '%s %s ptr [rbx]' % (op, PTR[w]))
    if w > 8:
        add('imul %s, %s' % (a, b), 'imul %s, %s' % (a, a), 'imul %s, %s, -3' % (a, b),
            'imul %s, %s, 0x7f' % (a, a), 'imul %s, %s ptr [rbx], 1000' % (a, PTR[w]))
        for op in ('cmove', 'cmovne', 'cmovb', 'cmova', 'cmovl', 'cmovg', 'cmovp', 'cmovs', 'cmovo'):
            add('%s %s, %s' % (op, a, b), '%s %s, %s ptr [rbx]' % (op, a, PTR[w]))
        add('xchg %s, %s' % (a, b), 'xadd %s, %s' % (a, b), 'cmpxchg %s, %s' % (a, b),
            'xadd %s ptr [rbx], %s' % (PTR[w], b), 'cmpxchg %s ptr [rbx], %s' % (PTR[w], b),
            'lock cmpxchg %s ptr [rbx], %s' % (PTR[w], b), 'bswap %s' % a if w >= 32 else 'nop')
    for op in ('adc', 'sbb', 'add', 'sub', 'cmp', 'and', 'or', 'xor', 'test'):
        add('%s %s, %s' % (op, a, b), '%s %s, %s' % (op, a, a), '%s %s, 1' % (op, a),
            '%s %s ptr [rbx], %s' % (op, PTR[w], b))
add('cmpxchg byte ptr [rbx], cl', 'lock cmpxchg byte ptr [rbx], dl', 'lock cmpxchg word ptr [rbx], dx',
    'cmpxchg8b qword ptr [rbx]', 'cmpxchg16b xmmword ptr [rbx]', 'xchg byte ptr [rbx], al',
    'xchg ah, al', 'xchg eax, eax', 'xchg ax, ax', 'xchg rax, rcx')
add('movsx eax, ah', 'movsx rax, bh', 'movzx eax, ah', 'movzx ecx, ch', 'movsx ax, al', 'movsx rax, ax',
    'movzx rax, word ptr [rbx]', 'movsxd rax, dword ptr [rbx]', 'movsxd rax, eax', 'movsx eax, byte ptr [rbx]')
add('lea eax, [rax + rcx*8 + 0x7fffffff]', 'lea ax, [rbx + rsi]', 'lea eax, [ebx + esi*2 + 5]',
    'lea rax, [eax + ecx]', 'lea ecx, [rcx + rcx*4]', 'lea r8d, [r8 - 1]')
add('cbw', 'cwde', 'cdqe', 'cwd', 'cdq', 'cqo', 'sahf', 'lahf', 'cmc', 'clc', 'stc')
for cc in ('o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g'):
    add('set%s al' % cc, 'set%s ah' % cc, 'set%s byte ptr [rbx]' % cc)
add('movsb', 'movsw', 'movsd', 'movsq', 'stosb', 'stosq', 'lodsw', 'scasb', 'cmpsb', 'cmpsq',
    'rep movsb', 'rep stosw', 'rep stosq', 'repe cmpsb', 'repne scasb', 'repe scasd', 'rep movsd')

# ---- SSE compares: every predicate, every width, reg/inplace/mem ----
PRED = ['eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord']
for p in PRED:
    for s, mw in (('ps', 'xmmword'), ('pd', 'xmmword'), ('ss', 'dword'), ('sd', 'qword')):
        add('cmp%s%s xmm1, xmm2' % (p, s), 'cmp%s%s xmm3, xmm3' % (p, s),
            'cmp%s%s xmm9, %s ptr [rbx]' % (p, s, mw), 'cmp%s%s xmm0, xmm12' % (p, s))
for op in ('ucomiss', 'comiss'):
    add('%s xmm0, xmm1' % op, '%s xmm2, xmm2' % op, '%s xmm3, dword ptr [rbx]' % op)
for op in ('ucomisd', 'comisd'):
    add('%s xmm0, xmm1' % op, '%s xmm2, xmm2' % op, '%s xmm3, qword ptr [rbx]' % op)

# ---- SSE float arithmetic / min / max / sqrt / approx ----
for op in ('add', 'sub', 'mul', 'div', 'min', 'max', 'sqrt'):
    for s, mw in (('ps', 'xmmword'), ('pd', 'xmmword'), ('ss', 'dword'), ('sd', 'qword')):
        add('%s%s xmm1, xmm2' % (op, s), '%s%s xmm4, xmm4' % (op, s), '%s%s xmm5, %s ptr [rbx]' % (op, s, mw),
            '%s%s xmm10, xmm3' % (op, s))
for op in ('rcpps', 'rsqrtps'):
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op)
for op in ('rcpss', 'rsqrtss'):
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, dword ptr [rbx]' % op)
for op in ('andps', 'andnps', 'orps', 'xorps', 'andpd', 'andnpd', 'orpd', 'xorpd',
           'pand', 'pandn', 'por', 'pxor'):
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op)

# ---- moves ----
add('movss xmm1, xmm2', 'movss xmm3, xmm3', 'movss xmm4, dword ptr [rbx]', 'movss dword ptr [rbx], xmm5',
    'movsd xmm1, xmm2', 'movsd xmm3, xmm3', 'movsd xmm4, qword ptr [rbx]', 'movsd qword ptr [rbx], xmm5',
    'movhlps xmm1, xmm2', 'movhlps xmm3, xmm3', 'movlhps xmm1, xmm2', 'movlhps xmm3, xmm3',
    'movlps xmm1, qword ptr [rbx]', 'movlps qword ptr [rbx], xmm2', 'movhps xmm1, qword ptr [rbx]',
    'movhps qword ptr [rbx], xmm2', 'movlpd xmm1, qword ptr [rbx]', 'movhpd xmm1, qword ptr [rbx]',
    'movlpd qword ptr [rbx], xmm2', 'movhpd qword ptr [rbx], xmm2',
    'movd xmm1, eax', 'movd eax, xmm1', 'movd xmm2, dword ptr [rbx]', 'movd dword ptr [rbx], xmm2',
    'movq xmm1, rax', 'movq rax, xmm1', 'movq xmm1, xmm2', 'movq xmm3, xmm3', 'movq xmm2, qword ptr [rbx]',
    'movq qword ptr [rbx], xmm2', 'movaps xmm1, xmm2', 'movups xmmword ptr [rbx], xmm3',
    'movmskps eax, xmm1', 'movmskps rax, xmm2', 'movmskpd eax, xmm1', 'movmskpd rcx, xmm3',
    'pmovmskb eax, xmm1', 'pmovmskb rcx, xmm2', 'movnti dword ptr [rbx], eax', 'movnti qword ptr [rbx], rcx',
    'movntps xmmword ptr [rbx], xmm1', 'movntdq xmmword ptr [rbx], xmm1', 'movntpd xmmword ptr [rbx], xmm1')

# ---- conversions ----
for x in ('cvtsi2ss', 'cvtsi2sd'):
    add('%s xmm1, eax' % x, '%s xmm1, rax' % x, '%s xmm2, dword ptr [rbx]' % x, '%s xmm2, qword ptr [rbx]' % x,
        '%s xmm9, r10d' % x, '%s xmm9, r10' % x)
for x in ('cvttss2si', 'cvtss2si'):
    add('%s eax, xmm1' % x, '%s rax, xmm1' % x, '%s ecx, dword ptr [rbx]' % x, '%s rcx, dword ptr [rbx]' % x,
        '%s r9d, xmm11' % x)
for x in ('cvttsd2si', 'cvtsd2si'):
    add('%s eax, xmm1' % x, '%s rax, xmm1' % x, '%s ecx, qword ptr [rbx]' % x, '%s rcx, qword ptr [rbx]' % x)
for x, mw in (('cvtss2sd', 'dword'), ('cvtsd2ss', 'qword'), ('cvtdq2ps', 'xmmword'), ('cvtps2dq', 'xmmword'),
              ('cvttps2dq', 'xmmword'), ('cvtps2pd', 'qword'), ('cvtpd2ps', 'xmmword'), ('cvtdq2pd', 'qword'),
              ('cvtpd2dq', 'xmmword'), ('cvttpd2dq', 'xmmword')):
    add('%s xmm1, xmm2' % x, '%s xmm3, xmm3' % x, '%s xmm4, %s ptr [rbx]' % (x, mw))
add('cvtpi2ps xmm1, mm2', 'cvtps2pi mm1, xmm2', 'cvttps2pi mm1, xmm2', 'cvtpi2pd xmm1, mm2',
    'cvtpd2pi mm1, xmm2', 'cvttpd2pi mm1, xmm2')

# ---- shuffles / unpacks / packs (immediates that exercise every selector) ----
for imm in (0x00, 0x1B, 0x4E, 0xB1, 0xE4, 0xFF, 0x39, 0x93, 0x8D):
    add('shufps xmm1, xmm2, %d' % imm, 'shufps xmm3, xmm3, %d' % imm, 'shufps xmm4, xmmword ptr [rbx], %d' % imm,
        'pshufd xmm1, xmm2, %d' % imm, 'pshufd xmm3, xmm3, %d' % imm, 'pshufd xmm4, xmmword ptr [rbx], %d' % imm,
        'pshuflw xmm1, xmm2, %d' % imm, 'pshuflw xmm3, xmm3, %d' % imm,
        'pshufhw xmm1, xmm2, %d' % imm, 'pshufhw xmm3, xmm3, %d' % imm,
        'pshufw mm1, mm2, %d' % imm, 'pshufw mm3, mm3, %d' % imm, 'pshufw mm4, qword ptr [rbx], %d' % imm)
for imm in range(4):
    add('shufpd xmm1, xmm2, %d' % imm, 'shufpd xmm3, xmm3, %d' % imm, 'shufpd xmm4, xmmword ptr [rbx], %d' % imm)
for op in ('unpcklps', 'unpckhps', 'unpcklpd', 'unpckhpd', 'punpcklbw', 'punpckhbw', 'punpcklwd', 'punpckhwd',
           'punpckldq', 'punpckhdq', 'punpcklqdq', 'punpckhqdq', 'packsswb', 'packssdw', 'packuswb'):
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op, '%s xmm8, xmm1' % op)

# ---- packed integer arithmetic (SSE and MMX, reg / inplace / mem) ----
PINT = ['paddb', 'paddw', 'paddd', 'paddq', 'psubb', 'psubw', 'psubd', 'psubq',
        'paddsb', 'paddsw', 'psubsb', 'psubsw', 'paddusb', 'paddusw', 'psubusb', 'psubusw',
        'pcmpeqb', 'pcmpeqw', 'pcmpeqd', 'pcmpgtb', 'pcmpgtw', 'pcmpgtd',
        'pminub', 'pmaxub', 'pminsw', 'pmaxsw', 'pmullw', 'pmulhw', 'pmulhuw', 'pmaddwd',
        'pavgb', 'pavgw', 'pmuludq', 'psadbw', 'pand', 'pandn', 'por', 'pxor']
for op in PINT:
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op,
        '%s mm1, mm2' % op, '%s mm3, mm3' % op, '%s mm4, qword ptr [rbx]' % op)
for op in ('punpcklbw', 'punpckhbw', 'punpcklwd', 'punpckhwd', 'punpckldq', 'punpckhdq',
           'packsswb', 'packssdw', 'packuswb'):
    add('%s mm1, mm2' % op, '%s mm3, mm3' % op)
    add('%s mm4, %s ptr [rbx]' % (op, 'dword' if 'punpckl' in op else 'qword'))
for op in ('pcmpeqq', 'pminsb', 'pmaxsb', 'pminuw', 'pmaxuw', 'pminsd', 'pmaxsd', 'pminud', 'pmaxud', 'pshufb'):
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op)
add('pshufb mm1, mm2')

# ---- packed shifts: every count class, imm / register / memory count ----
for op in ('psllw', 'pslld', 'psllq', 'psrlw', 'psrld', 'psrlq', 'psraw', 'psrad'):
    for cnt in (0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 255):
        add('%s xmm1, %d' % (op, cnt), '%s mm1, %d' % (op, cnt))
    add('%s xmm1, xmm2' % op, '%s xmm3, xmm3' % op, '%s xmm4, xmmword ptr [rbx]' % op,
        '%s mm1, mm2' % op, '%s mm3, mm3' % op, '%s mm4, qword ptr [rbx]' % op)
for op in ('pslldq', 'psrldq'):
    for cnt in (0, 1, 4, 7, 8, 15, 16, 17, 255):
        add('%s xmm1, %d' % (op, cnt), '%s xmm9, %d' % (op, cnt))

# ---- word insert / extract ----
for imm in (0, 1, 3, 4, 7, 8, 9, 255):
    add('pextrw eax, xmm1, %d' % imm, 'pextrw eax, mm1, %d' % imm, 'pinsrw xmm1, eax, %d' % imm,
        'pinsrw mm1, eax, %d' % imm, 'pinsrw xmm2, word ptr [rbx], %d' % imm, 'pinsrw mm2, word ptr [rbx], %d' % imm)
add('pextrw rax, xmm1, 2', 'pinsrw xmm1, rax, 2')

# ---- MMX moves ----
add('movd mm1, eax', 'movd eax, mm1', 'movd mm2, dword ptr [rbx]', 'movd dword ptr [rbx], mm2',
    'movq mm1, rax', 'movq rax, mm1', 'movq mm1, mm2', 'movq mm3, mm3', 'movq mm2, qword ptr [rbx]',
    'movq qword ptr [rbx], mm2', 'movntq qword ptr [rbx], mm3', 'movq2dq xmm1, mm2', 'movdq2q mm1, xmm2',
    'pmovmskb eax, mm1', 'pmovmskb rcx, mm2', 'maskmovq mm1, mm2', 'emms')


def main():
    out = sys.argv[1]
    src = '.intel_syntax noprefix\n.text\n' + '\n'.join(x for x in L if x != 'nop') + '\n'
    with tempfile.TemporaryDirectory() as td:
        s = os.path.join(td, 'suite.s')
        o = os.path.join(td, 'suite.o')
        b = os.path.join(td, 'suite.bin')
        open(s, 'w').write(src)
        r = subprocess.run(['as', '--64', '-o', o, s], capture_output=True, text=True)
        if r.returncode:
            # drop lines the assembler rejects and retry
            bad = set()
            for line in r.stderr.splitlines():
                if ':' in line and 'Error' in line:
                    try:
                        bad.add(int(line.split(':')[1]))
                    except ValueError:
                        pass
            lines = src.splitlines()
            print('[suite] assembler rejected %d lines, e.g.: %s' % (len(bad), [lines[i - 1] for i in sorted(bad)][:8]))
            src = '\n'.join(l for i, l in enumerate(lines, 1) if i not in bad) + '\n'
            open(s, 'w').write(src)
            subprocess.run(['as', '--64', '-o', o, s], check=True)
        subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', o, b], check=True)
        code = open(b, 'rb').read()
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    forms = {}
    n = 0
    for (addr, sz, mn, ops) in md.disasm_lite(code, 0):
        n += 1
        k = form_key(mn, ops)
        if k not in forms:
            forms[k] = [0, code[addr:addr + sz].hex(), 0]
    json.dump({'pe': 'dt_suite', 'instructions': n, 'forms': forms}, open(out, 'w'))
    print('[suite] %d instructions, %d distinct forms' % (n, len(forms)))


if __name__ == '__main__':
    main()
