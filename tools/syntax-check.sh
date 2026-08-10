#!/usr/bin/env bash
#
# Cross-platform syntax check.
#
# MacTab targets MSVC on Windows, but it is developed on macOS where none of it
# can be compiled or run. This script parses and type-checks the Win32-only
# sources against the mingw-w64 headers so that typos, bad API signatures, wrong
# argument counts and similar mistakes are caught before the code ever reaches a
# Windows machine.
#
# What this DOES catch: syntax errors, unknown identifiers, type mismatches,
# wrong Win32 signatures, missing includes.
#
# What it does NOT catch: anything in the Composition rendering layer (the
# C++/WinRT projection headers ship only with the Windows SDK), MSVC-specific
# pragmas, and of course all runtime behaviour. A clean run here means "this
# will probably compile", not "this works".
#
#   ./tools/syntax-check.sh          check every eligible source
#   ./tools/syntax-check.sh src/x.cpp  check one file
#
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

MINGW_PREFIX="$(brew --prefix mingw-w64 2>/dev/null || echo /opt/homebrew/opt/mingw-w64)"
CXX="$MINGW_PREFIX/toolchain-x86_64/bin/x86_64-w64-mingw32-g++"

if [[ ! -x "$CXX" ]]; then
    echo "error: mingw-w64 cross compiler not found at $CXX" >&2
    echo "       install it with:  brew install mingw-w64" >&2
    exit 127
fi

# Sources that include <winrt/...> cannot be checked here; list them so the
# skip is explicit and visible rather than silent.
WINRT_ONLY=(
    "src/panel.cpp"
)

is_winrt_only() {
    local f="${1#"$ROOT"/}"
    for skip in "${WINRT_ONLY[@]}"; do
        [[ "$f" == "$skip" ]] && return 0
    done
    return 1
}

FLAGS=(
    -fsyntax-only
    -std=c++20
    -municode
    -Wall -Wextra
    -Wno-unknown-pragmas
    -Wno-cast-function-type
    -I "$ROOT/src"
    -I "$ROOT/res"
    -DUNICODE -D_UNICODE
    -DNOMINMAX -DWIN32_LEAN_AND_MEAN
    -DMACTAB_VERSION='"0.1.0"'
    # mingw defaults to an older target; MacTab needs Windows 10 declarations
    # (DWMWA_*, DPI_AWARENESS_CONTEXT, NOTIFYICON_VERSION_4, ...).
    -D_WIN32_WINNT=0x0A00
    -DWINVER=0x0A00
    -DNTDDI_VERSION=0x0A000007
)

if [[ $# -gt 0 ]]; then
    FILES=("$@")
else
    FILES=()
    while IFS= read -r f; do FILES+=("$f"); done < <(find "$ROOT/src" -name '*.cpp' | sort)
fi

fail=0
checked=0
skipped=0

for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || { echo "skip (missing): $f"; continue; }

    if is_winrt_only "$f"; then
        echo "SKIP  ${f#"$ROOT"/}  (WinRT/Composition — MSVC only)"
        skipped=$((skipped + 1))
        continue
    fi

    if "$CXX" "${FLAGS[@]}" "$f" 2>&1 | sed "s|$ROOT/||"; then
        echo "ok    ${f#"$ROOT"/}"
        checked=$((checked + 1))
    else
        echo "FAIL  ${f#"$ROOT"/}"
        fail=$((fail + 1))
    fi
done

echo
echo "checked $checked, skipped $skipped, failed $fail"
exit $((fail > 0 ? 1 : 0))
