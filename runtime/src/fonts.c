/*
 * fonts.c - an "Arial" in C:\Windows\Fonts.
 *
 * Unity's built-in UI font (LegacyRuntime) has no glyph data of its own: on Windows it loads the
 * OS font whose family name is Arial from C:\Windows\Fonts. House Party's menus use it for most of
 * their labels, which stayed empty here. At start the runtime writes a copy of a font of this
 * system (Roboto/DroidSans on Android, DejaVu on Linux) with a new 'name' table that calls it
 * Arial into <user dir>/C/Windows/Fonts (where C:\Windows\Fonts maps to). The glyphs differ from
 * Arial; the layout is close enough for menu labels.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <sys/stat.h>
#include <unistd.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void pbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void pbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* 'name' table: format 0, ids 1 (family), 2 (subfamily), 4 (full), 6 (PostScript); Windows (3,1,0x409)
 * UTF-16BE and Macintosh (1,0,0) ASCII records. */
static size_t build_name(uint8_t *out, const char *const strs[4])
{
    static const uint16_t ids[4] = { 1, 2, 4, 6 };
    uint16_t count = 8, so = (uint16_t)(6 + 12 * count);
    uint8_t *rec = out + 6, *str = out + so;
    size_t off = 0;
    pbe16(out, 0); pbe16(out + 2, count); pbe16(out + 4, so);
    for (int pass = 0; pass < 2; pass++)                       /* Mac first: records sorted by platform */
        for (int k = 0; k < 4; k++) {
            const char *s = strs[k];
            size_t n = strlen(s), len = pass ? n * 2 : n;
            pbe16(rec, pass ? 3 : 1); pbe16(rec + 2, pass ? 1 : 0); pbe16(rec + 4, pass ? 0x409 : 0);
            pbe16(rec + 6, ids[k]); pbe16(rec + 8, (uint16_t)len); pbe16(rec + 10, (uint16_t)off);
            for (size_t i = 0; i < n; i++) {
                if (pass) { str[off + 2 * i] = 0; str[off + 2 * i + 1] = (uint8_t)s[i]; }
                else str[off + i] = (uint8_t)s[i];
            }
            off += len; rec += 12;
        }
    return so + off;
}

static int write_renamed(const char *src, const char *dst, const char *const strs[4])
{
    FILE *f = fopen(src, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 12 || n > (64 << 20)) { fclose(f); return -1; }
    size_t cap = (size_t)n + 4096;
    uint8_t *b = calloc(1, cap);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
    fclose(f);
    uint32_t ver = be32(b);
    if (ver != 0x00010000u && ver != 0x74727565u /* 'true' */ && ver != 0x4F54544Fu /* 'OTTO' */) { free(b); return -1; }
    uint16_t nt = be16(b + 4);
    uint8_t *rec = NULL;
    for (int i = 0; i < nt; i++)
        if (!memcmp(b + 12 + 16 * i, "name", 4)) rec = b + 12 + 16 * i;
    if (!rec) { free(b); return -1; }
    size_t at = ((size_t)n + 3) & ~(size_t)3;
    size_t len = build_name(b + at, strs);
    uint32_t sum = 0;
    for (size_t i = 0; i < ((len + 3) & ~(size_t)3); i += 4) sum += be32(b + at + i);
    pbe32(rec + 4, sum); pbe32(rec + 8, (uint32_t)at); pbe32(rec + 12, (uint32_t)len);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", dst);
    FILE *o = fopen(tmp, "wb");
    if (!o) { free(b); return -1; }
    size_t total = at + ((len + 3) & ~(size_t)3);
    int ok = fwrite(b, 1, total, o) == total;
    fclose(o);
    free(b);
    if (!ok || rename(tmp, dst)) { unlink(tmp); return -1; }
    return 0;
}

static const char *first_existing(const char *const *cands)
{
    for (; *cands; cands++) if (access(*cands, R_OK) == 0) return *cands;
    return NULL;
}

void w64_fonts_init(void)
{
    char dir[1200], p[1300];
    snprintf(dir, sizeof dir, "%s/C", w64_userdir); mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/C/Windows", w64_userdir); mkdir(dir, 0777);
    snprintf(dir, sizeof dir, "%s/C/Windows/Fonts", w64_userdir); mkdir(dir, 0777);
    static const char *const regular[] = {
        "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", NULL };
    static const char *const bold[] = {
        "/system/fonts/DroidSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", NULL };
    static const char *const nr[4] = { "Arial", "Regular", "Arial", "ArialMT" };
    static const char *const nb[4] = { "Arial", "Bold", "Arial Bold", "Arial-BoldMT" };
    const char *r = getenv("W64_FONT_REGULAR") ? getenv("W64_FONT_REGULAR") : first_existing(regular);
    const char *bd = getenv("W64_FONT_BOLD") ? getenv("W64_FONT_BOLD") : first_existing(bold);
    struct stat st;
    snprintf(p, sizeof p, "%s/arial.ttf", dir);
    if (r && stat(p, &st)) w64_log("[fonts] %s from %s: %s", p, r, write_renamed(r, p, nr) ? "failed" : "ok");
    snprintf(p, sizeof p, "%s/arialbd.ttf", dir);
    if (bd && stat(p, &st)) w64_log("[fonts] %s from %s: %s", p, bd, write_renamed(bd, p, nb) ? "failed" : "ok");
    if (!r) w64_log("[fonts] no system font found for Arial");
}
