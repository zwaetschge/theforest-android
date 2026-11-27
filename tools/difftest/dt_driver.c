/*
 * dt_driver.c - Linux driver for the difftest64 port.
 *
 * Per case: copy the original instruction bytes (+ RET) into an executable page,
 * run them natively from a seeded register file (dt_native.S), run the lifted C
 * from the same state, and compare GPRs (not RSP), the six modelled flags (minus
 * the per-mnemonic undefined mask), xmm0-15, mm0-7 and the scratch buffer.
 *
 * Differences from the Windows original:
 *   - SEH __try/__except -> sigsetjmp/siglongjmp on SIGSEGV/SIGBUS/SIGILL/SIGFPE/
 *     SIGTRAP, on an alternate signal stack.
 *   - The code page and the scratch buffer are ONE MAP_32BIT mapping
 *     ([code 4K][scratch 16K][guard 4K]) so that (a) RIP-relative operands can be
 *     retargeted into the scratch buffer with a fixed displacement and
 *     (b) 32-bit (0x67) address forms see a valid pointer.
 *   - Raw mismatches are written as binary records (dtrecords.bin) for the
 *     Python side to classify (undefined flags by count, NaN payloads, ...).
 *   - A watchdog (SIGALRM) and an "unarmed signal" path print the case index and
 *     exit, so the Python driver can drop the case and resume.
 */
#define _GNU_SOURCE
#include "cpu64.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/mman.h>
#include "dtseeds.h"
#include "dtcases.h"

int64_t g_image_delta = 0;
__thread uint8_t *w64_teb;
static uint8_t fake_teb[0x2000];
void w64_todo(uint64_t va, const char *text) { (void)va; (void)text; }
void dispatch(CPU *c, uint64_t t) { (void)c; (void)t; }
void dispatch_jmp(CPU *c, uint64_t t) { (void)c; (void)t; }
void es3_eh_push(es3_ehframe_t *f, uint64_t func, jmp_buf *jb) { (void)f; (void)func; (void)jb; }
void es3_eh_pop(es3_ehframe_t *f) { (void)f; }

DTSTATE dt_in __attribute__((aligned(64)));
DTSTATE dt_out __attribute__((aligned(64)));
void *dt_code;
extern void run_native(void);

#define FMASK ((1u<<0)|(1u<<2)|(1u<<4)|(1u<<6)|(1u<<7)|(1u<<11))
#define CODE_SIZE    0x1000u
#define SCRATCH_SIZE 0x4000u
#define MID_OFF      0x2000u          /* scratch midpoint, from SCRATCH_BASE */

static unsigned char *MAP, *PAGE, *SCRATCH_BASE, *SCRATCH_MID;
static unsigned char scratch_native[SCRATCH_SIZE];

static sigjmp_buf jb;
static volatile sig_atomic_t armed;     /* 1 native, 2 lifted */
static volatile int cur_case = -1;

static void on_sig(int sig, siginfo_t *si, void *uc)
{
    (void)si; (void)uc;
    if (armed) { armed = 0; siglongjmp(jb, sig); }
    char buf[96];
    int n = snprintf(buf, sizeof buf, "\nCRASH %d sig %d\n", cur_case, sig);
    if (write(1, buf, n) < 0) {}
    _exit(3);
}

static void on_alarm(int sig)
{
    (void)sig;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "\nHANG %d\n", cur_case);
    if (write(1, buf, n) < 0) {}
    _exit(4);
}

static uint64_t pack_cpu_flags(const CPU *c)
{
    return (c->cf) | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7) | (c->of << 11);
}
static void unpack_to_cpu(CPU *c, uint64_t f)
{
    c->cf = f & 1; c->pf = (f >> 2) & 1; c->af = (f >> 4) & 1;
    c->zf = (f >> 6) & 1; c->sf = (f >> 7) & 1; c->of = (f >> 11) & 1;
}
static uint64_t *cpu_gpr(CPU *c, int i)
{
    switch (i) {
    case 0: return &c->rax; case 1: return &c->rcx; case 2: return &c->rdx;
    case 3: return &c->rbx; case 4: return &c->rsp; case 5: return &c->rbp;
    case 6: return &c->rsi; case 7: return &c->rdi; case 8: return &c->r8;
    case 9: return &c->r9; case 10: return &c->r10; case 11: return &c->r11;
    case 12: return &c->r12; case 13: return &c->r13; case 14: return &c->r14;
    default: return &c->r15;
    }
}

/* Even trials tile the seed's xmm file over the buffer (so memory operands see
 * the same float/int edge values as the registers); odd trials use the
 * original's arithmetic pattern, which also exposes a read of the wrong
 * address. Not zero-filled: a wrong-width read must show. */
static void scratch_fill(int trial, const DTSTATE *s)
{
    if (trial & 1) {
        for (unsigned k = 0; k < SCRATCH_SIZE; k++)
            SCRATCH_BASE[k] = (unsigned char)(k * 31u + trial * 7u + 0x5Au);
    } else {
        const unsigned char *t = (const unsigned char *)s->xmm;
        for (unsigned k = 0; k < SCRATCH_SIZE; k++)
            SCRATCH_BASE[k] = t[(k + (unsigned)trial * 8u) & 255u];
    }
}

static long first_diff(const unsigned char *a, const unsigned char *b, unsigned n)
{
    for (unsigned k = 0; k < n; k++) if (a[k] != b[k]) return (long)k;
    return -1;
}

#define APPROX_REL_ERR (1.5 / 4096.0)
static int float_within_estimate(float a, float b)
{
    if (memcmp(&a, &b, 4) == 0) return 1;
    if (a != a && b != b) return 1;
    if (a != a || b != b) return 0;
    double d = (double)a - (double)b, m = (double)(b < 0 ? -b : b);
    if (d < 0) d = -d;
    if (m == 0.0) return d == 0.0;
    if (b == __builtin_inff() || b == -__builtin_inff()) return a == b;
    return (d / m) <= APPROX_REL_ERR;
}

typedef struct {
    int32_t idx, trial, kind, aux;   /* kind 1 mismatch, 2 lifted fault (aux = signal) */
    DTSTATE in, nat, lif;
    uint8_t memnat[32], memlif[32];  /* at aux (mismatch) = first differing scratch offset */
} REC;

static FILE *frec;

static void state_from_cpu(DTSTATE *o, const CPU *c)
{
    memset(o, 0, sizeof *o);
    for (int k = 0; k < 16; k++) o->gpr[k] = *cpu_gpr((CPU *)c, k);
    o->flags = pack_cpu_flags(c);
    memcpy(o->xmm, c->xmm, sizeof o->xmm);
    for (int k = 0; k < 8; k++) o->mm[k] = c->mm[k].u64[0];
}

static const DTSTATE *seed_for(int i, int trial)
{
    if (trial < NSTRUCT) return &seeds[trial];
    return &seeds[NSTRUCT + ((unsigned)(i * 48 + (trial - NSTRUCT)) % (NSEEDS - NSTRUCT))];
}

int main(int argc, char **argv)
{
    int start = argc > 1 ? atoi(argv[1]) : 0;
    const char *recpath = argc > 2 ? argv[2] : "dtrecords.bin";
    setvbuf(stdout, NULL, _IONBF, 0);
    w64_teb = fake_teb;

    MAP = mmap(NULL, CODE_SIZE + SCRATCH_SIZE + 0x1000, PROT_READ | PROT_WRITE | PROT_EXEC,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (MAP == MAP_FAILED) { perror("mmap"); return 2; }
    mprotect(MAP + CODE_SIZE + SCRATCH_SIZE, 0x1000, PROT_NONE);       /* guard */
    PAGE = MAP;
    SCRATCH_BASE = MAP + CODE_SIZE;
    SCRATCH_MID = SCRATCH_BASE + MID_OFF;
    if ((uintptr_t)(SCRATCH_MID - PAGE) != RIP_TARGET_OFF) { printf("layout mismatch\n"); return 2; }

    static unsigned char altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_sig;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP };
    for (unsigned k = 0; k < sizeof sigs / sizeof sigs[0]; k++) sigaction(sigs[k], &sa, NULL);
    signal(SIGALRM, on_alarm);

    frec = fopen(recpath, start ? "ab" : "wb");
    if (!frec) { perror("records"); return 2; }

    for (int i = start; i < dt_ncases; i++) {
        const CASE *tc = &dt_cases[i];
        cur_case = i;
        alarm(3);
        memset(PAGE, 0xCC, CODE_SIZE);
        memcpy(PAGE, tc->code, tc->len);
        PAGE[tc->len] = 0xC3;
        dt_code = PAGE;
        int pass = 0, fail = 0, nfault = 0, lfault = 0;

        for (int trial = 0; trial < NTRIALS; trial++) {
            DTSTATE in = *seed_for(i, trial);
            for (int k = 0; k < 16; k++) {
                if (tc->basem & (1u << k)) in.gpr[k] = (uint64_t)(uintptr_t)(SCRATCH_MID + tc->dadj);
                if (tc->idxm & (1u << k)) in.gpr[k] = (uint64_t)((in.gpr[k] & 3u) * (16u / (unsigned)tc->scale));
                if (tc->smallm & (1u << k)) in.gpr[k] = in.gpr[k] & 0x3Fu;
                if (tc->bitoffm & (1u << k)) in.gpr[k] = (uint64_t)((int64_t)(in.gpr[k] & 0x3FFu) - 0x200);
            }
            /* Page-edge trial: the memory operand ends exactly at the guard page,
             * so a lifted access WIDER than the instruction's (an over-read the
             * value comparison cannot see) faults on the lifted side only. */
            if (tc->msize && !tc->rip && (trial & 7) == 7) {
                for (int k = 0; k < 16; k++) {
                    if (tc->basem & (1u << k))
                        in.gpr[k] = (uint64_t)(uintptr_t)(SCRATCH_BASE + SCRATCH_SIZE - tc->msize) - (uint64_t)(int64_t)tc->dsp;
                    if (tc->idxm & (1u << k)) in.gpr[k] = 0;
                }
            }
            if (tc->div) {            /* make the dividend's high half valid */
                int sz = tc->div & 0xF, sg = tc->div & 0x10;
                uint64_t rax = in.gpr[0];
                if (sz == 1) in.gpr[0] = (rax & ~(uint64_t)0xFF00) | (sg && (rax & 0x80) ? 0xFF00 : 0);
                else if (sz == 2) in.gpr[2] = (in.gpr[2] & ~(uint64_t)0xFFFF) | (sg && (rax & 0x8000) ? 0xFFFF : 0);
                else if (sz == 4) in.gpr[2] = (in.gpr[2] & ~(uint64_t)0xFFFFFFFF) | (sg && (rax & 0x80000000u) ? 0xFFFFFFFFu : 0);
                else in.gpr[2] = (sg && (rax >> 63)) ? ~(uint64_t)0 : 0;
            }
            in.flags = (in.flags & FMASK) | 0x202;

            /* ---- native ---- */
            scratch_fill(trial, &in);
            dt_in = in;
            memset(&dt_out, 0, sizeof dt_out);
            if (sigsetjmp(jb, 1) == 0) {
                armed = 1;
                run_native();
                armed = 0;
            } else {
                __asm__ volatile("emms; cld" ::: "memory");
                nfault++;
                continue;
            }
            memcpy(scratch_native, SCRATCH_BASE, SCRATCH_SIZE);

            /* ---- lifted ---- */
            scratch_fill(trial, &in);
            static CPU c;
            memset(&c, 0, sizeof c);
            for (int k = 0; k < 16; k++) *cpu_gpr(&c, k) = in.gpr[k];
            memcpy(c.xmm, in.xmm, sizeof c.xmm);
            for (int k = 0; k < 8; k++) { c.mm[k].u64[0] = in.mm[k]; c.mm[k].u64[1] = 0xA5A5A5A5DEADBEEFull ^ (uint64_t)k; }
            unpack_to_cpu(&c, in.flags);
            c.mxcsr = 0x1F80;
            g_image_delta = tc->rip ? (int64_t)((uintptr_t)PAGE) - (int64_t)0x140000000LL : 0;
            int lsig;
            if ((lsig = sigsetjmp(jb, 1)) == 0) {
                armed = 2;
                tc->fn(&c);
                armed = 0;
            } else {
                g_image_delta = 0;
                lfault++;
                REC r; memset(&r, 0, sizeof r);
                r.idx = i; r.trial = trial; r.kind = 2; r.aux = lsig;
                r.in = in; r.nat = dt_out; state_from_cpu(&r.lif, &c);
                fwrite(&r, sizeof r, 1, frec);
                continue;
            }
            g_image_delta = 0;

            /* ---- compare ---- */
            int bad = 0;
            long md = -1;
            if (memcmp(scratch_native, SCRATCH_BASE, SCRATCH_SIZE) != 0) {
                md = first_diff(scratch_native, SCRATCH_BASE, SCRATCH_SIZE);
                bad = 1;
            }
            for (int k = 0; k < 16; k++)
                if (k != 4 && *cpu_gpr(&c, k) != dt_out.gpr[k]) bad = 1;
            uint64_t fm = FMASK & ~(uint64_t)tc->undef;
            if ((dt_out.flags & fm) != (pack_cpu_flags(&c) & fm)) bad = 1;
            for (int k = 0; k < 16; k++) {
                if (memcmp(&c.xmm[k], &dt_out.xmm[k], 16) == 0) continue;
                if (tc->approx) {
                    int ok = 1;
                    for (int l = 0; l < 4; l++) {
                        if (tc->approx == 2 || l == 0) ok &= float_within_estimate(c.xmm[k].f32[l], dt_out.xmm[k].f32[l]);
                        else ok &= c.xmm[k].u32[l] == dt_out.xmm[k].u32[l];
                    }
                    if (ok) continue;
                }
                bad = 1;
            }
            for (int k = 0; k < 8; k++)
                if (c.mm[k].u64[0] != dt_out.mm[k]) bad = 1;
            if (!bad) { pass++; continue; }
            fail++;
            REC r; memset(&r, 0, sizeof r);
            r.idx = i; r.trial = trial; r.kind = 1; r.aux = (int32_t)md;
            r.in = in; r.nat = dt_out; state_from_cpu(&r.lif, &c);
            if (md >= 0) {
                long o = md & ~15L;
                if (o + 32 > (long)SCRATCH_SIZE) o = SCRATCH_SIZE - 32;
                memcpy(r.memnat, scratch_native + o, 32);
                memcpy(r.memlif, SCRATCH_BASE + o, 32);
                r.aux = (int32_t)md;
            }
            fwrite(&r, sizeof r, 1, frec);
        }
        alarm(0);
        fflush(frec);
        printf("S %d %d %d %d %d\n", i, pass, fail, nfault, lfault);
    }
    fclose(frec);
    printf("DONE %d\n", dt_ncases);
    return 0;
}
