#!/bin/sh
# Build out/TheForest-Android-<version>.apk in Docker from your own PC copy of The Forest.
#   ./build.sh "/path/to/The Forest"   (folder containing TheForest.exe + TheForest_Data/)
# Optional env: JOBS=8, FORCE=1, IMAGE=theforest-build,
#               TF_KEYSTORE, TF_KEYSTORE_PASSWORD, TF_KEY_ALIAS, TF_KEY_PASSWORD
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
IN=${1:?usage: ./build.sh </path/to/The Forest>}
IMAGE=${IMAGE:-theforest-build}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "== building Docker image $IMAGE (first run only)"
  docker build -t "$IMAGE" "$ROOT/docker"
fi

MOUNT=$(cd "$IN" && pwd)

exec docker run --rm \
  -v "$ROOT":/w \
  -v "$MOUNT":/game:ro \
  -w /w \
  -e JOBS="${JOBS:-$(nproc 2>/dev/null || echo 8)}" \
  -e FORCE="${FORCE:-}" \
  -e TF_KEYSTORE="${TF_KEYSTORE:-}" \
  -e TF_KEYSTORE_PASSWORD="${TF_KEYSTORE_PASSWORD:-}" \
  -e TF_KEY_ALIAS="${TF_KEY_ALIAS:-}" \
  -e TF_KEY_PASSWORD="${TF_KEY_PASSWORD:-}" \
  "$IMAGE" sh tools/build.sh /game
