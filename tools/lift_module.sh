#!/bin/sh
# Lift one rebased module: tools/lift_module.sh <Name> (ref/lift/<Name>.dll|.exe, catalog work/cat/<Name>.txt)
# -> gen/<Name>/ (only changed files replaced, so unchanged translation units are not recompiled)
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
M=$1
PE=ref/lift/$M.dll; [ -f $PE ] || PE=ref/lift/$M.exe
OUT=gen/$M; NEW=gen.new/$M
mkdir -p $OUT work/lift
if [ -n "$COPYONLY" ]; then
  n=0; for f in $NEW/*; do b=$(basename $f); if ! cmp -s $f $OUT/$b; then cp $f $OUT/$b; n=$((n+1)); fi; done
  rm -rf $NEW; echo "$M: $n files changed"; exit 0
fi
rm -rf $NEW; mkdir -p gen.new
PYTHONHASHSEED=0 PYTHONPATH=$ROOT/third_party/pcrecomp python3 third_party/pcrecomp/tools/lift/generate64.py $PE work/cat/$M.txt $NEW > work/lift/$M.log 2>&1 \
  || { tail -20 work/lift/$M.log; exit 1; }
python3 tools/directcalls.py $NEW config/keep_dispatch_$M.txt >> work/lift/$M.log
python3 tools/shadowcpu.py $NEW config/o0_units_$M.txt >> work/lift/$M.log
# NOCOPY=1: leave the result in gen.new/<M> (e.g. while a build reads gen/); copy later with COPYONLY=1
[ -n "$NOCOPY" ] && { echo "$M: lifted into $NEW (not copied)"; exit 0; }
n=0
for f in $NEW/*; do b=$(basename $f); if ! cmp -s $f $OUT/$b; then cp $f $OUT/$b; n=$((n+1)); fi; done
rm -rf $NEW
echo "$M: $n files changed"; grep -E "recomp_todo_sites|lift_errors|functions  " work/lift/$M.log
