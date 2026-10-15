#!/usr/bin/env bash
# Loads the built HelixSR Scaler under Wine, the way Proton does on Linux and Steam Deck, after installing it with
# setup_linux.sh: dxgi.dll must load, log as HelixSR Scaler, find the HelixSR folder and write helixsr.ini.
# No GPU here, so nothing is drawn; this checks the Linux route up to the point a game starts rendering.
#
#   helixsr/tests/test_wine.sh PACKAGE_FOLDER      (needs wine and x86_64-w64-mingw32-gcc)
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PKG=$(realpath "$1")
W=$(mktemp -d)
trap 'wineserver -k 2>/dev/null; rm -rf "$W"' EXIT
fails=0
check() { if eval "$2"; then printf 'ok   %s\n' "$1"; else printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); fi; }

G="$W/game"
mkdir -p "$G" "$W/home/Downloads/HelixSR"
x86_64-w64-mingw32-gcc -O2 -o "$G/TestGame.exe" "$HERE/wine_load.c" || exit 1
# Stand-in for HelixSR's built DLL (only the network trailer is checked before the game runs)
{ head -c 65536 /dev/urandom; printf 'HXSRNET1'; head -c 24 /dev/zero; } >"$W/home/Downloads/HelixSR/amd_fidelityfx_dx12.dll"

HOME="$W/home" "$PKG/setup_linux.sh" --yes --game "$G" </dev/null >"$W/setup.txt" 2>&1
check "setup_linux.sh installs" '[ -f "$G/dxgi.dll" ] && [ -f "$G/HelixSR/helixsr.dll" ]'
sed -i 's/^LogToFile=auto/LogToFile=true/; s/^WaveSize=auto/WaveSize=64/; s/^Int8=auto/Int8=on/; s/^FrameGeneration=auto/FrameGeneration=off/' \
    "$G/OptiScaler.ini"
# stand-in for HelixSR's frame generation folder (what HelixSR Desktop copies when the setup built it)
mkdir -p "$G/HelixSR/helixsr-fg" && echo stand-in >"$G/HelixSR/helixsr-fg/nvexec64.dll"

export WINEPREFIX="$W/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="dxgi=n,b;mscoree,mshtml="
wineboot -i >/dev/null 2>&1
(cd "$G" && timeout 180 wine TestGame.exe >"$W/run.txt" 2>&1)
echo "wine exit code: $?"
cat "$W/run.txt"

check "dxgi.dll loads from the game folder" 'grep -qi "loaded from .*game.dxgi.dll" "$W/run.txt"'
check "OptiScaler.log written" '[ -s "$G/OptiScaler.log" ]'
check "logs as HelixSR Scaler" 'grep -q "HelixSR Scaler v" "$G/OptiScaler.log"'
check "finds the HelixSR folder" 'grep -q "HelixSR found in" "$G/OptiScaler.log"'
check "writes helixsr.ini from OptiScaler.ini" 'grep -q "^WaveSize = 64" "$G/HelixSR/helixsr.ini" && grep -q "^Int8 = on" "$G/HelixSR/helixsr.ini"'
check "HelixSR's own sharpener off" 'grep -q "^Sharpener = off" "$G/HelixSR/helixsr.ini"'
check "FrameGeneration=off parks helixsr-fg" '[ ! -e "$G/HelixSR/helixsr-fg" ] && [ -f "$G/HelixSR/helixsr-fg.off/nvexec64.dll" ]'
check "no frame generation set up when off" '! grep -q "FGInput = nvngxfg" "$G/OptiScaler.log"'

# switched back on: the folder comes back on the next start
sed -i 's/^FrameGeneration=off/FrameGeneration=on/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FrameGeneration=on brings helixsr-fg back" '[ -f "$G/HelixSR/helixsr-fg/nvexec64.dll" ] && [ ! -e "$G/HelixSR/helixsr-fg.off" ]'
# a GPU without 32-lane waves (RX 580): HelixSR's log says so, HelixSR Scaler switches frame generation off for good
sed -i 's/^FrameGeneration=on/FrameGeneration=auto/' "$G/OptiScaler.ini"
printf '[13:52:43.487] fg: frame generation NOT available on this GPU: no 32-lane waves (e.g. AMD Vega / Polaris); (waves 64-64 lanes); the game is told it is unavailable\r\n' \
    >"$G/HelixSR/helixsr.log"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "unsupported GPU: frame generation off by itself" '[ -d "$G/HelixSR/helixsr-fg.off" ] && [ ! -e "$G/HelixSR/helixsr-fg" ]'
check "unsupported GPU: saved in OptiScaler.ini" 'grep -q "^FrameGeneration=off" "$G/OptiScaler.ini"'
check "no second copy of HelixSR's DLL (one DLL for DLSS)" '[ ! -e "$G/HelixSR/helixsr_upscaler.dll" ]'

if [ $fails != 0 ] && [ -f "$G/OptiScaler.log" ]; then
    echo "--- OptiScaler.log (first 80 lines)"
    head -n 80 "$G/OptiScaler.log"
fi
echo
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
