#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
"${CXX:-x86_64-w64-mingw32-g++}" -std=c++20 -O2 -Wall -Wextra -Werror -static \
    "$HERE/resource_states.cpp" -o "$WORK/resource_states.exe"
WINEDEBUG=-all "${WINE:-wine}" "$WORK/resource_states.exe"
