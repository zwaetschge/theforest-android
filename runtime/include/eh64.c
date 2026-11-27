/*
 * eh64.c - see eh64.h.
 *
 * Two halves. The first is an ordinary x64 virtual unwind, run over GUEST
 * registers and the GUEST stack using the original image's .pdata/.xdata: the
 * lifter pushes real return addresses and executes the original prologues, so
 * the frames the unwind data describes are the frames that are really there.
 * The second reads the FuncInfo behind __CxxFrameHandler3 to decide which of
 * those frames catches.
 *
 * Everything is read out of the loaded guest image, so no table has to be
 * generated alongside the lifted code and none can go stale against it.
 */

#include "cpu64.h"

#include <stdio.h>
#include <string.h>

uint64_t es3_eh_image_base;     /* the executable; kept for compatibility */
int      g_eh_trace;

/* Several lifted modules (runtime/src/modules.c): every frame is read against the module its pc
 * is in, and the throw's type information against the module that threw. */
extern int w64_eh_module(uint64_t va, uint64_t *base, uint32_t *pdata_rva, uint32_t *pdata_n);
static __thread uint64_t eh_base;
extern void dispatch(CPU *c, uint64_t target);
extern int  es3_cs_depth_get(void);
extern void es3_cs_depth_set(int d);

static __thread es3_ehframe_t *eh_top;

void es3_eh_push(es3_ehframe_t *f, uint64_t func, jmp_buf *jb)
{
    f->prev = eh_top;
    f->jb = jb;
    f->func = func;
    f->resume = 0;
    f->cs_depth = es3_cs_depth_get();
    eh_top = f;
}

void es3_eh_pop(es3_ehframe_t *f) { eh_top = f->prev; }

/* ---- image access ---------------------------------------------------------
 * RVAs, because every pointer in unwind and EH data is one. */
static uint8_t *img(uint32_t rva)
{
    return (uint8_t *)(eh_base + rva);
}

static uint32_t ru32(uint32_t rva)
{
    uint32_t v;
    memcpy(&v, img(rva), 4);
    return v;
}

static uint16_t ru16(uint32_t rva)
{
    uint16_t v;
    memcpy(&v, img(rva), 2);
    return v;
}

static int32_t ri32(uint32_t rva)
{
    int32_t v;
    memcpy(&v, img(rva), 4);
    return v;
}

typedef struct { uint32_t begin, end, unwind; } rfunc_t;

/* Selects the module of va for img()/ru32(); its function table in *pd / *pd_n. */
static int eh_select(uint64_t va, rfunc_t **pd, uint32_t *pd_n)
{
    uint64_t base; uint32_t rva, n;
    if (!w64_eh_module(va, &base, &rva, &n)) return 0;
    eh_base = base;
    if (pd) *pd = (rfunc_t *)(uintptr_t)(base + rva);
    if (pd_n) *pd_n = n;
    return 1;
}

static rfunc_t *pdata_find(uint64_t pc)
{
    rfunc_t *pd; uint32_t n;
    if (!eh_select(pc, &pd, &n) || !n) return 0;
    uint32_t rva = (uint32_t)(pc - eh_base);
    int lo = 0, hi = (int)n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (rva < pd[m].begin) hi = m - 1;
        else if (rva >= pd[m].end) lo = m + 1;
        else return &pd[m];
    }
    return 0;
}

/* ---- virtual unwind -------------------------------------------------------
 * Unwinds one guest frame: restores the non-volatile registers it saved, pops
 * the return address, and leaves *pc and c->rsp describing its caller.
 *
 * The non-volatiles matter as much as rsp does. The lifted body executed the
 * `push rbx` and never reached the matching `pop`, so a frame that catches
 * without this resumes with a callee's value in its own register - which does
 * not fault, and is wrong in a way nothing reports.
 */
static uint64_t *gpr(CPU *c, int n) { return &((uint64_t *)c)[n]; }

/* Where this frame's locals are measured from: rsp once the prologue has run,
 * which is what rsp already is at a call site in the body - unless the function
 * keeps a frame register, and then it is that. Every EH offset (the catch
 * object, the unwind help slot) is relative to this. */
static uint64_t frame_establisher(CPU *c, const rfunc_t *rf)
{
    uint32_t u;
    uint8_t fr, freg, foff, ncodes;

    if (!rf) return c->rsp;
    u = rf->unwind;
    for (;;) {
        uint8_t ver_flags = *img(u);
        ncodes = *img(u + 2);
        fr = *img(u + 3);
        freg = fr & 0xf;
        foff = fr >> 4;
        if (freg) return *gpr(c, freg) - (uint64_t)foff * 16;
        if (!(ver_flags & 0x20)) break;                 /* UNW_FLAG_CHAININFO */
        u = ru32(u + 4 + (((uint32_t)ncodes + 1) & ~1u) * 2 + 8);
    }
    return c->rsp;
}

static int unwind_one(CPU *c, uint64_t *pc, uint64_t *establisher)
{
    rfunc_t *rf = pdata_find(*pc);
    uint32_t rva = (uint32_t)(*pc - eh_base);
    uint32_t u, off;
    uint64_t frame_base;

    if (!rf) {                          /* leaf: only a return address */
        if (establisher) *establisher = c->rsp;
        *pc = rd64(c->rsp);
        c->rsp += 8;
        return *pc != 0;
    }
    u = rf->unwind;
    off = rva - rf->begin;
    frame_base = c->rsp;

    for (;;) {
        uint8_t ver_flags = *img(u);
        uint8_t ncodes = *img(u + 2);
        uint8_t fr = *img(u + 3);
        uint8_t freg = fr & 0xf, foff = fr >> 4;
        uint32_t codes = u + 4;
        int i;

        /* A frame register pins the establisher frame independently of how far
         * into the prologue we are, which is what the EH offsets are relative
         * to. */
        if (freg) frame_base = *gpr(c, freg) - (uint64_t)foff * 16;

        for (i = 0; i < ncodes; ) {
            uint8_t coff = *img(codes + i * 2);
            uint8_t opinfo = *img(codes + i * 2 + 1);
            uint8_t op = opinfo & 0xf, info = opinfo >> 4;
            int slots = 1;
            int apply = coff <= off;

            switch (op) {
            case 0:                                     /* PUSH_NONVOL */
                if (apply) {
                    *gpr(c, info) = rd64(c->rsp);
                    c->rsp += 8;
                }
                break;
            case 1:                                     /* ALLOC_LARGE */
                if (info == 0) {
                    slots = 2;
                    if (apply) c->rsp += (uint64_t)ru16(codes + (i + 1) * 2) * 8;
                } else {
                    slots = 3;
                    if (apply) c->rsp += ru32(codes + (i + 1) * 2);
                }
                break;
            case 2:                                     /* ALLOC_SMALL */
                if (apply) c->rsp += (uint64_t)info * 8 + 8;
                break;
            case 3:                                     /* SET_FPREG */
                if (apply) c->rsp = *gpr(c, freg) - (uint64_t)foff * 16;
                break;
            case 4:                                     /* SAVE_NONVOL */
                slots = 2;
                if (apply)
                    *gpr(c, info) =
                        rd64(frame_base + (uint64_t)ru16(codes + (i + 1) * 2) * 8);
                break;
            case 5:                                     /* SAVE_NONVOL_FAR */
                slots = 3;
                if (apply)
                    *gpr(c, info) = rd64(frame_base + ru32(codes + (i + 1) * 2));
                break;
            case 8: slots = 2; break;                   /* SAVE_XMM128 */
            case 9: slots = 3; break;                   /* SAVE_XMM128_FAR */
            default: break;                             /* PUSH_MACHFRAME */
            }
            i += slots;
        }
        if (ver_flags & 0x20) {                         /* UNW_FLAG_CHAININFO */
            uint32_t chain = u + 4 + (((uint32_t)ncodes + 1) & ~1u) * 2;
            u = ru32(chain + 8);
            off = 0xffffffffu;                          /* parent codes all apply */
            continue;
        }
        break;
    }
    if (establisher) *establisher = c->rsp;
    *pc = rd64(c->rsp);
    c->rsp += 8;
    return *pc != 0;
}

/* ---- __CxxFrameHandler3 data ---------------------------------------------- */
typedef struct {
    uint32_t magic;
    int32_t  max_state;
    int32_t  unwind_map;
    int32_t  n_try;
    int32_t  try_map;
    int32_t  n_ip;
    int32_t  ip_map;
    int32_t  unwind_help;
} funcinfo_t;

/* Language-specific data RVA behind the handler of this unwind info (0 if none). */
static uint32_t lsda_at(uint32_t unwind)
{
    if (!unwind) return 0;
    uint8_t ver_flags = *img(unwind);
    if (!(ver_flags & 0x08)) return 0;                  /* no UNW_FLAG_EHANDLER */
    uint8_t ncodes = *img(unwind + 2);
    uint32_t hd = unwind + 4 + (((uint32_t)ncodes + 1) & ~1u) * 2;
    return ru32(hd + 4);
}

static int funcinfo_at(uint32_t unwind, funcinfo_t *fi)
{
    uint32_t rva = lsda_at(unwind);
    if (!rva) return 0;
    memcpy(fi, img(rva), sizeof *fi);
    /* 0x1993052x are the MSVC revisions; FH4 data has no magic (see fh4_*) */
    return (fi->magic & 0xfffffff0u) == 0x19930520u;
}

static int state_at(const funcinfo_t *fi, uint32_t rva)
{
    int i, st = -1;
    for (i = 0; i < fi->n_ip; i++) {
        if ((uint32_t)ri32(fi->ip_map + 8 * i) > rva) break;
        st = ri32(fi->ip_map + 8 * i + 4);
    }
    return st;
}

/* ---- __CxxFrameHandler4 (compressed) ---------------------------------------- */
static uint32_t fh4_u(uint32_t *p)                      /* compressed unsigned */
{
    static const uint8_t len[16] = { 1, 2, 1, 3, 1, 2, 1, 4, 1, 2, 1, 3, 1, 2, 1, 5 };
    static const uint8_t sh[16] = { 25, 18, 25, 11, 25, 18, 25, 4, 25, 18, 25, 11, 25, 18, 25, 0 };
    uint8_t lb = *img(*p) & 0x0F;
    uint32_t v = ru32(*p + len[lb] - 4) >> sh[lb];
    *p += len[lb];
    return v;
}
static int32_t fh4_i(uint32_t *p) { int32_t v = ri32(*p); *p += 4; return v; }

typedef struct { uint32_t trymap, ipmap; int ok; } fh4_t;
static fh4_t fh4_at(uint32_t unwind)
{
    fh4_t f = { 0, 0, 0 };
    uint32_t p = lsda_at(unwind);
    if (!p) return f;
    uint8_t hdr = *img(p); p++;
    if (hdr & 0x80) return f;
    if (hdr & 0x04) (void)fh4_u(&p);                    /* BBT flags */
    if (hdr & 0x08) (void)fh4_i(&p);                    /* unwind map */
    if (hdr & 0x10) f.trymap = (uint32_t)fh4_i(&p);
    if (hdr & 0x02) return f;                           /* separated code segments: not handled */
    f.ipmap = (uint32_t)fh4_i(&p);
    f.ok = f.trymap != 0;
    return f;
}
static int fh4_state(uint32_t ipmap, uint32_t func_off)
{
    uint32_t p = ipmap, n = fh4_u(&p), ip = 0;
    int st = -1;
    for (uint32_t i = 0; i < n && i < 100000; i++) {
        ip += fh4_u(&p);
        int s = (int)fh4_u(&p) - 1;
        if (ip > func_off) break;
        st = s;
    }
    return st;
}

static const char *td_name(uint64_t base, uint32_t td)
{
    return td ? (const char *)(uintptr_t)(base + td + 16) : 0;
}

/* Does a catch of type descriptor catch_td (in the catching module, cbase) accept the thrown
 * object? ti is the ThrowInfo RVA in the throwing module (tbase). */
static int catch_matches(uint64_t cbase, uint32_t catch_td, uint64_t tbase, uint32_t ti, uint32_t *size)
{
    if (!catch_td) { *size = 8; return 1; }             /* catch(...) */
    const char *want = td_name(cbase, catch_td);
    uint32_t cta; memcpy(&cta, (void *)(uintptr_t)(tbase + ti + 0x0c), 4);
    if (!cta || !want) return 0;
    uint32_t n; memcpy(&n, (void *)(uintptr_t)(tbase + cta), 4);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint32_t ct, td; memcpy(&ct, (void *)(uintptr_t)(tbase + cta + 4 + 4 * i), 4);
        memcpy(&td, (void *)(uintptr_t)(tbase + ct + 4), 4);
        const char *have = td_name(tbase, td);
        if (have && !strcmp(have, want)) { memcpy(size, (void *)(uintptr_t)(tbase + ct + 0x14), 4); return 1; }
    }
    return 0;
}

/* ---- the throw ------------------------------------------------------------ */
typedef struct { int found; uint32_t handler, obj_off, obj_size, adj; uint32_t ncont, cont[2], cont_rva; int catch_high; } catch_t;

static void find_catch_fh3(const funcinfo_t *fi, int st, uint64_t tbase, uint32_t ti, catch_t *r)
{
    int best_low = -1;
    for (int t = 0; t < fi->n_try; t++) {
        int32_t lo = ri32(fi->try_map + 20 * t), hi = ri32(fi->try_map + 20 * t + 4);
        int32_t ch = ri32(fi->try_map + 20 * t + 8), nc = ri32(fi->try_map + 20 * t + 12);
        uint32_t ha = (uint32_t)ri32(fi->try_map + 20 * t + 16);
        if (st < lo || st > hi || lo <= best_low) continue;
        for (int k = 0; k < nc; k++) {
            uint32_t td = (uint32_t)ri32(ha + 20 * k + 4), sz = 8;
            if (!catch_matches(eh_base, td, tbase, ti, &sz)) continue;
            best_low = lo;
            r->found = 1; r->adj = ru32(ha + 20 * k);
            r->handler = (uint32_t)ri32(ha + 20 * k + 12);
            r->obj_off = (uint32_t)ri32(ha + 20 * k + 8);
            r->obj_size = sz; r->ncont = 0; r->catch_high = ch;
            break;
        }
    }
}

static void find_catch_fh4(const fh4_t *f, int st, uint64_t tbase, uint32_t ti, catch_t *r)
{
    uint32_t p = f->trymap, ntry = fh4_u(&p);
    int best_low = -1;
    for (uint32_t t = 0; t < ntry && t < 4096; t++) {
        int lo = (int)fh4_u(&p), hi = (int)fh4_u(&p);
        int ch = (int)fh4_u(&p);                        /* catchHigh */
        uint32_t ha = (uint32_t)fh4_i(&p);
        if (st < lo || st > hi || lo <= best_low) continue;
        uint32_t q = ha, nh = fh4_u(&q);
        for (uint32_t k = 0; k < nh && k < 256; k++) {
            uint8_t h = *img(q); q++;
            uint32_t adj = 0, td = 0, objoff = 0;
            if (h & 1) adj = fh4_u(&q);
            if (h & 2) td = (uint32_t)fh4_i(&q);
            if (h & 4) objoff = fh4_u(&q);
            uint32_t handler = (uint32_t)fh4_i(&q);
            uint32_t ncont = (h >> 4) & 3, cont[2] = { 0, 0 };
            for (uint32_t j = 0; j < ncont && j < 2; j++) cont[j] = (h & 8) ? (uint32_t)fh4_i(&q) : fh4_u(&q);
            uint32_t sz = 8;
            if (r->found && lo <= best_low) continue;
            if (!catch_matches(eh_base, td, tbase, ti, &sz)) continue;
            best_low = lo;
            r->found = 1; r->adj = adj; r->handler = handler; r->obj_off = objoff; r->obj_size = sz;
            r->ncont = ncont; r->cont[0] = cont[0]; r->cont[1] = cont[1]; r->cont_rva = (h & 8) != 0;
            r->catch_high = ch;
            break;
        }
    }
}

static __thread uint8_t eh_objcopy[512];

/* Catch funclets run on a stack of their own whose return address is 0. A throw out of one
 * (IL2CPP's "rethrow as ...") has to go on in the frame the funclet belongs to, in the state of
 * its catch block, so the enclosing try blocks of that frame - and its callers - get to see it. */
typedef struct { uint64_t gpr[16]; uint64_t pc, ret_slot; int catch_high; } eh_active_t;
static __thread eh_active_t eh_active[64];
static __thread int eh_nactive;

int es3_eh_throw(CPU *c, uint64_t ret_pc, uint64_t obj, uint64_t throw_info)
{
    uint64_t pc = ret_pc, establisher = 0, tbase;
    uint32_t ti, dummy;
    int depth;

    if (!obj || !throw_info || !ret_pc) return 0;
    if (!w64_eh_module(throw_info, &tbase, &dummy, &dummy)) return 0;
    ti = (uint32_t)(throw_info - tbase);
    if (g_eh_trace)
        fprintf(stderr, "[eh] throw obj %#llx ti %#llx from %#llx, rsp %#llx\n",
                (unsigned long long)obj, (unsigned long long)throw_info,
                (unsigned long long)ret_pc, (unsigned long long)c->rsp);

    int crossed = 0, st_override = -2;
    for (depth = 0; depth < 1024 && pc; depth++) {
        rfunc_t *rf = pdata_find(pc);
        catch_t r = { 0 };
        int st = -1;
        if (rf) {
            uint32_t rva = (uint32_t)(pc - eh_base);
            funcinfo_t fi;
            /* Every pc here is a RETURN address, and a return address can fall into the next
             * state's range. */
            if (funcinfo_at(rf->unwind, &fi)) {
                st = st_override > -2 ? st_override : state_at(&fi, rva - 1);
                find_catch_fh3(&fi, st, tbase, ti, &r);
            } else {
                fh4_t f4 = fh4_at(rf->unwind);
                if (f4.ok) {
                    st = st_override > -2 ? st_override : fh4_state(f4.ipmap, rva - 1 - rf->begin);
                    find_catch_fh4(&f4, st, tbase, ti, &r);
                }
            }
        }
        st_override = -2;
        if (g_eh_trace)
            fprintf(stderr, "[eh]   frame %2d pc %#llx fn %#llx state %d %s\n", depth, (unsigned long long)pc,
                    (unsigned long long)(rf ? eh_base + rf->begin : 0), st, r.found ? "CATCHES" : "");
        if (r.found) {
            uint64_t base = eh_base, fn = base + rf->begin, cont;
            es3_ehframe_t *f;
            establisher = frame_establisher(c, rf);
            for (f = eh_top; f; f = f->prev)
                if (f->func == fn) break;
            if (!f) {
                fprintf(stderr, "[eh] %#llx catches at state %d but has no landing pad - not lifted with EH?\n",
                        (unsigned long long)fn, st);
                return 0;
            }
            /* The catch parameter is a local of the catching frame. The thrown object sits in the
             * throwing frames, below the stack the funclet is about to use: keep a copy. */
            uint32_t n = r.obj_size < sizeof eh_objcopy ? r.obj_size : sizeof eh_objcopy;
            memcpy(eh_objcopy, (void *)(uintptr_t)obj, n);
            if (r.obj_off) {
                if (r.adj & 8) {                        /* HT_IsReference: catch (T &) */
                    uint64_t p = (uint64_t)(uintptr_t)eh_objcopy;
                    memcpy((void *)(uintptr_t)(establisher + r.obj_off), &p, 8);
                } else memcpy((void *)(uintptr_t)(establisher + r.obj_off), eh_objcopy, n);
            }
            if (g_eh_trace)
                fprintf(stderr, "[eh] funclet %#llx frame %#llx catchobj +%#x size %u adj %#x\n",
                        (unsigned long long)(base + r.handler), (unsigned long long)establisher,
                        r.obj_off, r.obj_size, r.adj);
            /* The catch funclet is guest code: it reaches the parent's locals through rdx and
             * returns where to continue in rax (FH4 with continuations in the metadata: an index). */
            eh_nactive -= crossed;                      /* funclets this throw left for good */
            if (eh_nactive < 0) eh_nactive = 0;
            int slot = eh_nactive < 64 ? eh_nactive : -1;
            if (slot >= 0) {
                eh_active_t *a = &eh_active[slot];
                memcpy(a->gpr, c, sizeof a->gpr);
                a->pc = pc; a->catch_high = r.catch_high;
            }
            c->rdx = establisher;
            c->rcx = 0;
            c->rsp = (establisher - 0x400) & ~(uint64_t)15;
            push64(c, 0);
            if (slot >= 0) { eh_active[slot].ret_slot = c->rsp; eh_nactive = slot + 1; }
            dispatch(c, base + r.handler);
            if (slot >= 0) eh_nactive = slot;
            cont = c->rax;
            if (r.ncont && cont < r.ncont) cont = r.cont_rva ? base + r.cont[cont] : fn + r.cont[cont];
            c->rsp = establisher;
            if (g_eh_trace)
                fprintf(stderr, "[eh] caught in %#llx (state %d), resuming at %#llx\n",
                        (unsigned long long)fn, st, (unsigned long long)cont);
            f->resume = cont;
            c->rip = cont;
            es3_cs_depth_set(f->cs_depth);
            eh_top = f;
            longjmp(*f->jb, 1);
        }
        if (!unwind_one(c, &pc, &establisher)) {
            /* out of a catch funclet: continue in its parent frame */
            int k = eh_nactive - 1 - crossed;
            if (pc || k < 0 || eh_active[k].ret_slot != c->rsp - 8) break;
            memcpy(c, eh_active[k].gpr, sizeof eh_active[k].gpr);
            pc = eh_active[k].pc;
            st_override = eh_active[k].catch_high;
            crossed++;
            if (g_eh_trace)
                fprintf(stderr, "[eh]   leaving catch funclet -> parent pc %#llx state %d\n",
                        (unsigned long long)pc, st_override);
        }
    }
    if (g_eh_trace)
        fprintf(stderr, "[eh] no guest frame caught the throw\n");
    return 0;
}
