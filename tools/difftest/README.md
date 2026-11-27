# Differential test of the lifter (Linux port of pcrecomp's difftest64)

Runs each sampled instruction natively (dt_native.S) and as lifted C (lift64_cpu.py + runtime/include/cpu64.h)
from the same register state and compares GPRs, flags, xmm and mm registers.

    # in hp-gfx:1 with the project at /w; corpora are sampled from the lifted modules (game-derived, kept in work/)
    python3 tools/difftest/dt_corpus.py ...        # see run_all.sh for the full sequence
    sh tools/difftest/run_all.sh <snapshot>        # snapshot = work/difftest/snap_<name>/{lift,include}

2026-11-25: found packed float compares lifted as scalar (cmpps/cmppd), bt/bts/btr/btc bit-string addressing
and 16-bit offsets, rol/ror and shld/shrd count edge cases, MMX punpckl* m32 reads. After the fixes 14,972
encodings from UnityPlayer and GameAssembly pass.
