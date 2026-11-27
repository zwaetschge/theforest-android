#!/bin/sh
# usage: tools/hostrun_gfx.sh <name> <seconds> [-e VAR=value ...]
# Like hostrun.sh, with graphics: Xvfb + lavapipe (Vulkan on the CPU) in hp-gfx:1. A screenshot of
# the virtual screen every SHOT seconds (default 20) goes to work/shots/<name>_NNN.png.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$ROOT/tools/local.env" ] && . "$ROOT/tools/local.env"
name=$1; secs=$2; shift 2
docker rm -f hp-run-$name >/dev/null 2>&1
docker run --rm --name hp-run-$name -v "${HOST_ROOT:-$ROOT}":/w -w /w \
  -e W64_TIMEOUT=$secs -e W64_SCREEN=${W64_SCREEN:-1280x720} -e DISPLAY=:99 -e SHOT=${SHOT:-20} "$@" ${IMAGE:-hp-gfx:1} \
  sh -c 'Xvfb :99 -screen 0 ${W64_SCREEN}x24 >/dev/null 2>&1 & sleep 1; mkdir -p work/user work/shots; rm -f work/shots/'$name'_*.png
         ( i=0; while sleep $SHOT; do i=$((i+1)); import -display :99 -window root work/shots/'$name'_$(printf %03d $i).png 2>/dev/null; done ) &
         build/host/tfhost /w/ref/game /w/work/user > work/'$name'.log 2>&1; echo EXIT $? >> work/'$name'.log'
tail -40 "$ROOT/work/$name.log"
