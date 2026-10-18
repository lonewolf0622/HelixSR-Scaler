#!/usr/bin/env bash
# Builds and runs test_names.cpp (the upscaler list's names and order with HelixSR's FidelityFX DLL) with the host's
# C++ compiler: the rules in OptiScaler/helixsr/HelixSRNames.h have no Windows dependencies.
#
#   helixsr/tests/test_names.sh      (needs g++ or clang++)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CXX=${CXX:-$(command -v g++ || command -v clang++)}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
"$CXX" -std=c++20 -Wall -O1 -o "$W/test_names" "$HERE/test_names.cpp"
"$W/test_names"
