#!/bin/sh
# Linux debug host build: every gen/<Module>/*.c with its own table names, the runtime, Mono, DXVK, host/main.c
# -> build/host/tfhost. Incremental (objects newer than their source are kept). env: JOBS, OPT
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
J=${JOBS:-14}; O=${OPT:--O1}
python3 tools/gen_modules.py ref/lift/layout.json gen/modules_table.c
F="-mcx16 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -g0 -fno-unwind-tables -fno-asynchronous-unwind-tables -Iruntime/include"
mkdir -p build/host/rt
: > build/host/todo.txt
for d in gen/*/; do
  M=$(basename $d)
  mkdir -p build/host/$M
  for f in $d*.c; do
    o=build/host/$M/$(basename $f .c).o
    if [ ! -f $o ] || [ $f -nt $o ]; then
      u=$M/$(basename $f .c); OO=$O; grep -qx "$u" config/o0_host.txt 2>/dev/null && OO=-O0
      echo "$OO $f $o -I$d -Drecomp_dispatch_table=${M}_dispatch_table -Drecomp_dispatch_count=${M}_dispatch_count -Drecomp_imports=${M}_imports" >> build/host/todo.txt
    fi
  done
done
echo "$(wc -l < build/host/todo.txt) generated units to compile"
[ -s build/host/todo.txt ] && cat build/host/todo.txt | xargs -P $J -L 1 sh -c 'clang $0 '"$F"' -c $1 -o $2 $3 $4 $5 $6'
RT="-O1 -g -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -fno-strict-aliasing -ffp-contract=off -Iruntime/include -Iruntime/src -Ithird_party/mono -Ibuild/mono-host -Ithird_party/mono/eglib/src -Ibuild/mono-host/eglib/src"
for f in runtime/src/*.c runtime/include/eh64.c host/main.c gen/modules_table.c; do
  o=build/host/rt/$(basename $f .c).o
  if [ ! -f $o ] || [ $f -nt $o ] || [ runtime/src/w64.h -nt $o ] || [ runtime/include/cpu64.h -nt $o ]; then clang $RT -c $f -o $o; fi
done
if [ ! -f build/host/rt/mono_tramp.o ] || [ runtime/src/mono_tramp.S -nt build/host/rt/mono_tramp.o ]; then
  clang -O1 -g -c runtime/src/mono_tramp.S -o build/host/rt/mono_tramp.o
fi
# Direct3D 11 bridge over DXVK native (tools/build_dxvk.sh host builds build/dxvk-host/libhp_dxvk.a)
D=build/dxvk-src
DX="-DDXVK_NATIVE -DDXVK_PLATFORM_LINUX -DNOMINMAX -I$D -I$D/include -I$D/src/util -I$D/include/native -I$D/include/native/windows -I$D/include/native/directx -I$D/generated"
for f in gfx/hp_d3d11.cpp; do
  o=build/host/rt/$(basename $f .cpp).o
  if [ ! -f $o ] || [ $f -nt $o ]; then clang++ -std=c++17 -O1 -g -w $DX -c $f -o $o; fi
done
MH=build/mono-host
MONO_LIBS="-Wl,--start-group $MH/mono/mini/.libs/libmini-static.a $MH/mono/metadata/.libs/libmonoruntime-static.a $MH/mono/metadata/.libs/libmonoruntime-config.a $MH/mono/io-layer/.libs/libwapi.a $MH/mono/utils/.libs/libmonoutils.a $MH/libgc/.libs/libmonogc-static.a $MH/eglib/src/.libs/libeglib.a -Wl,--end-group"
clang++ -o build/host/tfhost build/host/rt/*.o $(ls build/host/*/recomp_*.o) build/dxvk-host/libhp_dxvk.a $MONO_LIBS -lvulkan -lX11 -lpthread -ldl -lm -lrt
ls -la build/host/tfhost
