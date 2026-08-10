#!/usr/bin/env bash
#
# Build and run the icon pipeline preview natively.
#
# image.cpp and squircle.cpp are free of windows.h precisely so this is
# possible: it is the only way to actually LOOK at what the squircle pipeline
# produces while developing away from a Windows machine.
#
#   ./tools/preview/build.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build-preview"

mkdir -p "$OUT"

c++ -std=c++20 -O2 -Wall -Wextra \
    -I "$ROOT/src" \
    "$ROOT/tools/preview/preview.cpp" \
    "$ROOT/src/image.cpp" \
    "$ROOT/src/squircle.cpp" \
    -lz \
    -o "$OUT/preview"

# Wipe the output first. Renders that no longer correspond to any current code
# path sit in here looking exactly like the ones that do, and this project's
# recurring failure is a verification artifact that has drifted from the thing it
# is supposed to be verifying.
rm -rf "$OUT/out"
mkdir -p "$OUT/out"
"$OUT/preview" "$OUT/out"
