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

# HelixSR's FidelityFX DLL without helixsr.dll: FSR w/Dx12 (HelixSR) for DirectX 11 and Vulkan games, AMD's FSR next to
# it for HelixSR's list. Stand-in DLL: nothing loads it here (no D3D12 device), only the defaults are checked.
mv "$G/HelixSR" "$W/HelixSR.away"
mkdir -p "$G/HelixSR-fsrfg"
cp "$W/home/Downloads/HelixSR/amd_fidelityfx_dx12.dll" "$G/HelixSR-fsrfg/amd_fidelityfx_dx12.dll"
sed -i 's/^FGInput=.*/FGInput=auto/; s/^FGOutput=.*/FGOutput=auto/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=auto/; s/^Dx11Upscaler=.*/Dx11Upscaler=auto/; s/^VulkanUpscaler=.*/VulkanUpscaler=auto/; s/^FfxDx12SRPath=.*/FfxDx12SRPath=auto/' "$G/OptiScaler.ini"
cp "$G/OptiScaler.ini" "$W/requested.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: HelixSR's FidelityFX DLL is the FFX upscaler module" 'grep -q "HelixSR.s FidelityFX DLL .*HelixSR-fsrfg.amd_fidelityfx_dx12.dll is the FFX upscaler module" "$G/OptiScaler.log"'
check "FSR DLL: DirectX 11 and Vulkan default to FSR w/Dx12 (HelixSR, index 0)" 'grep -q "upscalers Dx11=ffx_12, Vulkan=ffx_12 (FFX upscaler index 0)" "$G/OptiScaler.log"'
check "FSR DLL: AMD's FSR next to HelixSR's DLL for its list" 'cmp -s "$G/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$G/HelixSR-fsrfg/amd_fidelityfx_upscaler_dx12.amd.dll"'
check "FSR DLL: the defaults are not saved into OptiScaler.ini" 'cmp -s "$W/requested.ini" "$G/OptiScaler.ini"'
check "FSR DLL: no HelixSR DLSS route without helixsr.dll" '! grep -q "helixsr.dll in use" "$G/OptiScaler.log"'

# OptiScaler.ini's choices stay
sed -i 's/^Dx11Upscaler=.*/Dx11Upscaler=fsr22/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: a Dx11Upscaler set in OptiScaler.ini stays" 'grep -q "upscalers Dx11=fsr22, Vulkan=ffx_12" "$G/OptiScaler.log"'
sed -i 's/^Dx11Upscaler=.*/Dx11Upscaler=auto/; s/^FfxDx12SRPath=.*/FfxDx12SRPath=C:\\elsewhere\\amd_fidelityfx_upscaler_dx12.dll/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: another FfxDx12SRPath in OptiScaler.ini wins" 'grep -q "but FfxDx12SRPath picks" "$G/OptiScaler.log" && ! grep -q "upscalers Dx11=ffx_12" "$G/OptiScaler.log"'

# Neither HelixSR form: OptiScaler's own defaults
sed -i 's/^FfxDx12SRPath=.*/FfxDx12SRPath=auto/' "$G/OptiScaler.ini"
rm -rf "$G/HelixSR-fsrfg"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "no HelixSR: OptiScaler's defaults" 'grep -q "no HelixSR folder" "$G/OptiScaler.log" && ! grep -q "FFX upscaler module" "$G/OptiScaler.log"'
mv "$W/HelixSR.away" "$G/HelixSR"

if [ $fails != 0 ] && [ -f "$G/OptiScaler.log" ]; then
    echo "--- OptiScaler.log (first 80 lines)"
    head -n 80 "$G/OptiScaler.log"
fi
echo
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
