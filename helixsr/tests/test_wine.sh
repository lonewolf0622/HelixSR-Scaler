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
check "HelixSR uses the FidelityFX input route" 'grep -q "HelixSR input route: FidelityFX" "$G/OptiScaler.log"'
check "default backends use FidelityFX and the Dx11 bridge" 'grep -q "HelixSR backend defaults: dx12=FFX, dx11=FFX_on12" "$G/OptiScaler.log"'
check "AMD providers are installed beside HelixSR" 'cmp -s "$PKG/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$G/HelixSR/amd_fidelityfx_upscaler_dx12.amd.dll"'
check "writes helixsr.ini from OptiScaler.ini" 'grep -q "^WaveSize = 64" "$G/HelixSR/helixsr.ini" && grep -q "^Int8 = on" "$G/HelixSR/helixsr.ini"'
check "HelixSR's own sharpener off" 'grep -q "^Sharpener = off" "$G/HelixSR/helixsr.ini"'
check "only HelixSR frame generation is unavailable" 'grep -q "HelixSR frame generation is not included; alternative FG settings:" "$G/OptiScaler.log"'
check "leftover HelixSR engine files are untouched" '[ -f "$G/HelixSR/helixsr-fg/nvexec64.dll" ] && [ ! -e "$G/HelixSR/helixsr-fg.off" ]'

# Verify actual startup configuration for alternative FG routes and every supported provider.
route() {
    local input=$1 output=$2 provider=$3 expected=$4
    sed -i "s/^FGInput=.*/FGInput=$input/; s/^FGOutput=.*/FGOutput=$output/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=$provider/; s/^FrameGeneration=.*/FrameGeneration=on/" "$G/OptiScaler.ini"
    awk '{ sub(/\r$/, "") } /^\[/ { section=$0 } section=="[FrameGen]" && /^Enabled=/ { $0="Enabled=true" } { print }' "$G/OptiScaler.ini" >"$W/route.ini"
    mv "$W/route.ini" "$G/OptiScaler.ini"
    cp "$G/OptiScaler.ini" "$W/requested.ini"
    (cd "$G" && timeout 180 wine TestGame.exe >"$W/run.txt" 2>&1)
    check "$input/$output/$provider preserves alternative routing" "grep -q 'alternative FG settings: input=$input, output=$output, provider=$expected' \"\$G/OptiScaler.log\""
    check "$input/$output/$provider preserves saved choices" 'cmp -s "$W/requested.ini" "$G/OptiScaler.ini"'
    check "HelixSR engine never initialized" '! grep -q "HelixSR frame generation initialized" "$G/OptiScaler.log"'
}
# NVNGX provider selection applies to NvngxFG input or DLSSG output only.
route Upscaler FSRFG auto Nukems
route DLSSG FSRFG auto Nukems
route NvngxFG NoFG Nukems Nukems
route NvngxFG NoFG Arturs Arturs
route DLSSG DLSSG Combo Combo
route XeFG XeFG auto Nukems
route Upscaler DLSSG FFX FFX
route DLSSG DLSSG None None
route NvngxFG NoFG HelixSR FFX

# An old unsupported-GPU log must not disable alternative frame generation.
sed -i 's/^WaveSize=64/WaveSize=auto/' "$G/OptiScaler.ini"
printf '%s\r\n' 'fg: frame generation NOT available on this GPU: no 32-lane waves' >"$G/HelixSR/helixsr.log"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "old HelixSR GPU log leaves alternatives available" 'grep -q "alternative FG settings: input=NvngxFG, output=NoFG, provider=FFX" "$G/OptiScaler.log"'
check "auto wave size remains 32 lanes" 'grep -q "^WaveSize = 32" "$G/HelixSR/helixsr.ini" && grep -q "^WaveSize=auto" "$G/OptiScaler.ini"'
check "no second copy of HelixSR's DLL (one DLL for DLSS)" '[ ! -e "$G/HelixSR/helixsr_upscaler.dll" ]'

# Saved alternative upscalers must survive the change of HelixSR's input interface.
sed -i 's/^Dx12Upscaler=.*/Dx12Upscaler=xess/; s/^Dx11Upscaler=.*/Dx11Upscaler=fsr22/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >"$W/run.txt" 2>&1)
check "saved alternative upscalers remain selected" 'grep -q "HelixSR backend defaults: dx12=XeSS, dx11=FSR22" "$G/OptiScaler.log"'
# A non-RTX installation that previously selected HelixSR as DLSS migrates to FFX.
sed -i 's/^Dx12Upscaler=.*/Dx12Upscaler=dlss/; s/^Dx11Upscaler=.*/Dx11Upscaler=dlss/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >"$W/run.txt" 2>&1)
check "legacy HelixSR DLSS selection migrates to FidelityFX" 'grep -q "HelixSR backend defaults: dx12=FFX, dx11=FFX_on12" "$G/OptiScaler.log"'

if [ $fails != 0 ] && [ -f "$G/OptiScaler.log" ]; then
    echo "--- OptiScaler.log (first 80 lines)"
    head -n 80 "$G/OptiScaler.log"
fi
echo
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
