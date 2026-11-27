#!/bin/sh
# run_all.sh <snapshot-name> : difftest all corpora against work/difftest/snap_<name>
# (run inside hp-gfx:1 with the project mounted at /w)
set -e
cd /w/work/difftest
S=$1
export DT_LIFT_DIR=/w/work/difftest/snap_$S/lift DT_INC=/w/work/difftest/snap_$S/include
python3 difftest64_linux.py corpus_UnityPlayer.json /w/ref/lift/UnityPlayer.dll out_${S}_up | tail -1
python3 difftest64_linux.py corpus_suite.json /w/ref/lift/UnityPlayer.dll out_${S}_suite --k-sse 100000 --k-int 100000 | tail -1
python3 difftest64_linux.py corpus_GameAssembly.json /w/ref/lift/GameAssembly.dll out_${S}_ga --exclude corpus_UnityPlayer.json | tail -1
python3 dt_report.py out_${S}_up out_${S}_suite out_${S}_ga --out report_${S}.txt --json fail_${S}.json
