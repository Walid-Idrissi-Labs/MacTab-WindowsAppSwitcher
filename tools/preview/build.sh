#!/usr/bin/env bash
#
# Build and run the native previews.
#
# image.cpp, squircle.cpp, glass*.h and mission_layout.h are free of windows.h
# precisely so this is possible: it is the only way to actually LOOK at what
# MacTab draws while developing away from a Windows machine.
#
# Two binaries, because they answer different questions and a failure in one
# should not hide the other:
#
#   preview   the icon pipeline and the glass material
#   mission   the Mission Control arrangement
#
#   ./tools/preview/build.sh
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$ROOT/build-preview"

mkdir -p "$OUT"

c++ -std=c++20 -O2 -Wall -Wextra \
    -I "$ROOT/src" -I "$ROOT/tools/preview" \
    "$ROOT/tools/preview/preview.cpp" \
    "$ROOT/src/image.cpp" \
    "$ROOT/src/squircle.cpp" \
    -lz \
    -o "$OUT/preview"

c++ -std=c++20 -O2 -Wall -Wextra \
    -I "$ROOT/src" -I "$ROOT/tools/preview" \
    "$ROOT/tools/preview/mission.cpp" \
    "$ROOT/src/image.cpp" \
    -lz \
    -o "$OUT/mission"

# Wipe the output first. Renders that no longer correspond to any current code
# path sit in here looking exactly like the ones that do, and this project's
# recurring failure is a verification artifact that has drifted from the thing it
# is supposed to be verifying.
rm -rf "$OUT/out"
mkdir -p "$OUT/out"
"$OUT/preview" "$OUT/out"
"$OUT/mission" "$OUT/out"
