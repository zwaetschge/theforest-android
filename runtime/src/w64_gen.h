/*
 * w64_gen.h - what the runtime needs from the generated code, for any number of lifted modules.
 *
 * Every lifted module (gen/<Name>/) is compiled with its own names for the generated tables:
 *   -Drecomp_dispatch_table=<Name>_dispatch_table -Drecomp_dispatch_count=<Name>_dispatch_count
 *   -Drecomp_imports=<Name>_imports
 * and gen/modules_table.c (tools/gen_modules.py) lists them for the loader.
 */
#pragma once
#include "cpu64.h"

typedef struct { uint64_t va; void (*fn)(CPU *); } recomp_entry_t;
typedef struct { uint64_t slot; const char *dll; const char *name; } hle_import_t;

#define HLE_BASE   0xE5E30000ull
#define HLE_STRIDE 8
#define HLE_SENTINEL(i) (HLE_BASE + (uint64_t)(i) * HLE_STRIDE)

/* One lifted PE module: what the build knows about it. */
typedef struct {
    const char *name;                 /* module name as LoadLibrary / imports see it, e.g. "UnityPlayer.dll" */
    const char *file;                 /* path below the game directory */
    uint64_t base;                    /* the base it was rebased to before lifting */
    const recomp_entry_t *disp;
    const uint32_t *ndisp;
    const hle_import_t *imps;
    uint32_t nimps;
} w64_modspec_t;

extern const w64_modspec_t w64_modspecs[];
extern const int w64_nmodspecs;
