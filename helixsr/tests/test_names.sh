#!/usr/bin/env bash
# Builds and runs test_names.cpp (the upscaler list's names and order with HelixSR's FidelityFX DLL) and
# test_fg_route.cpp (the default frame-generation route per game) and test_fg_quality.cpp (frame generation's quality
# settings) with the host's C++ compiler: the rules in OptiScaler/helixsr/HelixSRNames.h, HelixSRFgRoute.h and
# HelixSRFgQuality.h have no Windows dependencies.
#
#   helixsr/tests/test_names.sh      (needs g++ or clang++)
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
CXX=${CXX:-$(command -v g++ || command -v clang++)}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
for t in test_names test_fg_route test_fg_quality; do
    "$CXX" -std=c++20 -Wall -O1 -o "$W/$t" "$HERE/$t.cpp"
    "$W/$t"
done
