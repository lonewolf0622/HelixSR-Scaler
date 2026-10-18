#!/usr/bin/env bash
# Loads the built HelixSR Scaler under Wine, the way Proton does on Linux and Steam Deck, after installing it with
# setup_linux.sh: dxgi.dll must load, log as HelixSR Scaler, find HelixSR and write helixsr.ini, and frame generation
# must be exactly what OptiScaler.ini says (HelixSR is a provider like Nukem's, nothing picks it). The first part runs
# an install from before 1.1.0 (HelixSR/helixsr.dll, the DLSS route), the second HelixSR-fsrfg as installed now.
# No GPU here, so nothing is drawn; this checks the Linux route up to the point a game starts rendering.
#
#   helixsr/tests/test_wine.sh PACKAGE_FOLDER      (needs wine and x86_64-w64-mingw32-gcc)
set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PKG=$(realpath "$1")
W=$(mktemp -d)
trap 'wineserver -k 2>/dev/null; rm -rf "$W"' EXIT
fails=0
# dfg KEY VALUE: helixsr.ini next to HelixSR's FidelityFX DLL has KEY = VALUE in [DlssFrameGeneration]
dfg() {
    awk -v k="$1" -v v="$2" '{sub(/\r$/, "")} /^\[/{s=$0} s=="[DlssFrameGeneration]" && $0==k" = "v{f=1} END{exit !f}' "$G/HelixSR-fsrfg/helixsr.ini"
}

# opt KEY VALUE: the game's OptiScaler.ini has KEY=VALUE in [HelixSR]
opt() {
    awk -v k="$1" -v v="$2" '{sub(/\r$/, "")} /^\[/{s=$0} s=="[HelixSR]" && $0==k"="v{f=1} END{exit !f}' "$G/OptiScaler.ini"
}

check() { if eval "$2"; then printf 'ok   %s\n' "$1"; else printf 'FAIL %s\n' "$1"; fails=$((fails + 1)); fi; }

# in a library folder like Steam's (steamapps/common): the game-file search stays inside the game
G="$W/steamapps/common/game"
mkdir -p "$G" "$W/home/Downloads/HelixSR"
x86_64-w64-mingw32-gcc -O2 -o "$G/TestGame.exe" "$HERE/wine_load.c" || exit 1
# Stand-in for HelixSR's built DLL (only the network trailer is checked before the game runs)
{ head -c 65536 /dev/urandom; printf 'HXSRNET1'; head -c 24 /dev/zero; } >"$W/home/Downloads/HelixSR/amd_fidelityfx_dx12.dll"

HOME="$W/home" "$PKG/setup_linux.sh" --yes --game "$G" </dev/null >"$W/setup.txt" 2>&1
check "setup_linux.sh installs" '[ -f "$G/dxgi.dll" ] && [ -f "$G/HelixSR-fsrfg/amd_fidelityfx_dx12.dll" ] && [ ! -e "$G/HelixSR" ]'
check "frame generation off by default (OptiScaler's auto)" 'grep -q "^FGInput=auto" "$G/OptiScaler.ini" && grep -q "^FGNvngxReplacement=auto" "$G/OptiScaler.ini" && ! grep -q "^FrameGeneration=" "$G/OptiScaler.ini"'
# an install from before 1.1.0: HelixSR's DLL as HelixSR/helixsr.dll
mv "$G/HelixSR-fsrfg" "$W/HelixSR-fsrfg.setup"
mkdir -p "$G/HelixSR" && cp "$W/HelixSR-fsrfg.setup/amd_fidelityfx_dx12.dll" "$G/HelixSR/helixsr.dll"
sed -i 's/^LogToFile=auto/LogToFile=true/; s/^WaveSize=auto/WaveSize=64/; s/^Int8=auto/Int8=on/' "$G/OptiScaler.ini"
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
check "OptiScaler's own defaults (no FG)" 'grep -q "frame generation settings: input=NoFG, output=NoFG, provider=Nukems" "$G/OptiScaler.log"'
check "helixsr-fg stays where it is" '[ -f "$G/HelixSR/helixsr-fg/nvexec64.dll" ] && [ ! -e "$G/HelixSR/helixsr-fg.off" ]'
check "HelixSR's frame generation installed (logged)" 'grep -q "HelixSR.s frame generation installed" "$G/OptiScaler.log"'

# helixsr-fg.off left by an earlier version (frame generation parked): renamed back once
mv "$G/HelixSR/helixsr-fg" "$G/HelixSR/helixsr-fg.off"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "helixsr-fg.off of an earlier version renamed back" '[ -f "$G/HelixSR/helixsr-fg/nvexec64.dll" ] && [ ! -e "$G/HelixSR/helixsr-fg.off" ]'

# Frame generation is what OptiScaler.ini says, as in OptiScaler (an earlier [HelixSR] FrameGeneration=on changes nothing)
sed -i '/^\[HelixSR\]/a FrameGeneration=on' "$G/OptiScaler.ini"
route() {
    local input=$1 output=$2 provider=$3 expected=$4
    sed -i "s/^FGInput=.*/FGInput=$input/; s/^FGOutput=.*/FGOutput=$output/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=$provider/" "$G/OptiScaler.ini"
    cp "$G/OptiScaler.ini" "$W/requested.ini"
    (cd "$G" && timeout 180 wine TestGame.exe >"$W/run.txt" 2>&1)
    check "$input/$output/$provider -> $expected" "grep -q 'frame generation settings: input=$expected' \"\$G/OptiScaler.log\""
    check "$input/$output/$provider: OptiScaler.ini unchanged" 'cmp -s "$W/requested.ini" "$G/OptiScaler.ini"'
    check "$input/$output/$provider: HelixSR engine never initialized" '! grep -q "HelixSR frame generation initialized" "$G/OptiScaler.log"'
}
route auto auto auto "NoFG, output=NoFG, provider=Nukems"
route Upscaler FSRFG auto "Upscaler, output=FSRFG, provider=Nukems"
route DLSSG FSRFG auto "DLSSG, output=FSRFG, provider=Nukems"
route XeFG XeFG auto "XeFG, output=XeFG, provider=Nukems"
route NvngxFG NoFG Nukems "NvngxFG, output=NoFG, provider=Nukems"
route NvngxFG NoFG Arturs "NvngxFG, output=NoFG, provider=Arturs"
route DLSSG DLSSG Combo "DLSSG, output=DLSSG, provider=Combo"
route Upscaler DLSSG FFX "Upscaler, output=DLSSG, provider=FFX"
route DLSSG DLSSG None "DLSSG, output=DLSSG, provider=None"
route NvngxFG NoFG HelixSR "NvngxFG, output=NoFG, provider=HelixSR"
route NvngxFG auto HelixSR "NvngxFG, output=NoFG, provider=HelixSR"
route Upscaler DLSSG HelixSR "Upscaler, output=DLSSG, provider=HelixSR"
route NvngxFG NoFG auto "NvngxFG, output=NoFG, provider=Nukems"
route NoFG NoFG HelixSR "NoFG, output=NoFG, provider=Nukems"
route NoFG NoFG auto "NoFG, output=NoFG, provider=Nukems"
sed -i '/^FrameGeneration=/d' "$G/OptiScaler.ini"

# the game's own frame generation files change nothing (no route by the game)
sed -i "s/^FGInput=.*/FGInput=auto/; s/^FGOutput=.*/FGOutput=auto/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=auto/" "$G/OptiScaler.ini"
game_fg() {
    local file=$1
    mkdir -p "$G/Engine/Plugins/Stand-in" && : >"$G/Engine/Plugins/Stand-in/$file"
    (cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
    check "game with $file -> OptiScaler's defaults" "grep -q 'frame generation settings: input=NoFG, output=NoFG, provider=Nukems' \"\$G/OptiScaler.log\""
}
game_fg libxess_fg.dll
game_fg amd_fidelityfx_framegeneration_dx12.dll
game_fg sl.dlss_g.dll
rm -rf "$G/Engine"

# No startup recovery any more: an old start marker left by an earlier version does not switch frame generation off
: >"$G/HelixSR/helixsr-dlssg.starting"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "old helixsr-dlssg.starting: frame generation not switched off" '! grep -q "previous startup was interrupted" "$G/OptiScaler.log"'
rm -f "$G/HelixSR/helixsr-dlssg.starting"

# A GPU that can't run HelixSR's frame generation (helixsr.log says so): nothing is switched or saved
sed -i 's/^WaveSize=64/WaveSize=auto/' "$G/OptiScaler.ini"
printf '%s\r\n' 'fg: frame generation NOT available on this GPU: no 32-lane waves' >"$G/HelixSR/helixsr.log"
cp "$G/OptiScaler.ini" "$W/requested.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "unsupported GPU: OptiScaler.ini unchanged, helixsr-fg in place" 'cmp -s "$W/requested.ini" "$G/OptiScaler.ini" && [ -d "$G/HelixSR/helixsr-fg" ]'
check "auto wave size remains 32 lanes" 'grep -q "^WaveSize = 32" "$G/HelixSR/helixsr.ini" && grep -q "^WaveSize=auto" "$G/OptiScaler.ini"'
check "no second copy of HelixSR's DLL (one DLL for DLSS)" '[ ! -e "$G/HelixSR/helixsr_upscaler.dll" ]'
rm -f "$G/HelixSR/helixsr.log"

# HelixSR's FidelityFX DLL without helixsr.dll: FSR w/Dx12 (HelixSR) for DirectX 11 and Vulkan games, AMD's FSR next to
# it for HelixSR's list. Stand-in DLL: nothing loads it here (no D3D12 device), only the defaults are checked.
mv "$G/HelixSR" "$W/HelixSR.away"
mv "$W/HelixSR-fsrfg.setup" "$G/HelixSR-fsrfg"
sed -i 's/^FGInput=.*/FGInput=auto/; s/^FGOutput=.*/FGOutput=auto/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=auto/; s/^Dx11Upscaler=.*/Dx11Upscaler=auto/; s/^VulkanUpscaler=.*/VulkanUpscaler=auto/; s/^FfxDx12SRPath=.*/FfxDx12SRPath=auto/' "$G/OptiScaler.ini"
cp "$G/OptiScaler.ini" "$W/requested.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: HelixSR's FidelityFX DLL is the FFX upscaler module" 'grep -q "HelixSR.s FidelityFX DLL .*HelixSR-fsrfg.amd_fidelityfx_dx12.dll is the FFX upscaler module" "$G/OptiScaler.log"'
check "FSR DLL: DirectX 12 defaults to FSR, DirectX 11 and Vulkan to FSR w/Dx12 (HelixSR, index 0)" 'grep -q "upscalers Dx12=ffx, Dx11=ffx_12, Vulkan=ffx_12 (FFX upscaler index 0)" "$G/OptiScaler.log"'
check "FSR DLL: AMD's FSR next to HelixSR's DLL for its list" 'cmp -s "$G/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$G/HelixSR-fsrfg/amd_fidelityfx_upscaler_dx12.amd.dll"'
check "FSR DLL: the defaults are not saved into OptiScaler.ini" 'cmp -s "$W/requested.ini" "$G/OptiScaler.ini"'
check "FSR DLL: no HelixSR DLSS route without helixsr.dll" '! grep -q "helixsr.dll in use" "$G/OptiScaler.log"'
check "FSR DLL: HelixSR's settings go to helixsr.ini next to its DLL" 'grep -q "^Sharpener = off" "$G/HelixSR-fsrfg/helixsr.ini"'
check "FSR DLL: no helixsr-fg there, no HelixSR frame generation" 'grep -q "HelixSR.s frame generation not installed" "$G/OptiScaler.log"'
mkdir -p "$G/HelixSR-fsrfg/helixsr-fg" && echo stand-in >"$G/HelixSR-fsrfg/helixsr-fg/hxfg64.dll"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL with helixsr-fg, OptiScaler.ini on auto: OptiScaler's defaults, folder in place" 'grep -q "frame generation settings: input=NoFG, output=NoFG, provider=Nukems" "$G/OptiScaler.log" && grep -q "HelixSR.s frame generation installed" "$G/OptiScaler.log" && [ -f "$G/HelixSR-fsrfg/helixsr-fg/hxfg64.dll" ]'
sed -i 's/^FGInput=.*/FGInput=nvngxfg/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=HelixSR/; s/^QuickFlow=.*/QuickFlow=on/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL with helixsr-fg, FGInput=nvngxfg + FGNvngxReplacement=HelixSR: HelixSR as the provider" 'grep -q "frame generation settings: input=NvngxFG, output=NoFG, provider=HelixSR" "$G/OptiScaler.log" && [ -f "$G/HelixSR-fsrfg/helixsr-fg/hxfg64.dll" ]'
check "FSR DLL: Light by default: QuickFlow off in [DlssFrameGeneration] (an old on is not used)" 'dfg QuickFlow off'
check "FSR DLL: Light by default: Preset = c in [DlssFrameGeneration]" 'dfg Preset c'
check "OptiScaler.ini stays as written (QuickFlow=on is not rewritten)" 'opt QuickFlow on'
grep -q '^FGPreset=' "$G/OptiScaler.ini" || sed -i 's/^QuickFlow=/FGPreset=auto\nQuickFlow=/' "$G/OptiScaler.ini"
# Frame generation quality: FGPreset / QuickFlow in OptiScaler.ini, Preset / QuickFlow in helixsr.ini
for t in "full fast full on" "full fastest full fastest" "full off full off" "c fastest c off" "c fast c off"; do
    set -- $t
    sed -i "s/^FGPreset=.*/FGPreset=$1/; s/^QuickFlow=.*/QuickFlow=$2/" "$G/OptiScaler.ini"
    (cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
    check "FGPreset=$1 QuickFlow=$2: helixsr.ini Preset = $3, QuickFlow = $4" "dfg Preset $3 && dfg QuickFlow $4"
    check "FGPreset=$1 QuickFlow=$2: OptiScaler.ini unchanged" "opt FGPreset $1 && opt QuickFlow $2"
done
sed -i 's/^QuickFlow=.*/QuickFlow=auto/; s/^FGPreset=.*/FGPreset=auto/; s/^FGInput=.*/FGInput=auto/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=auto/' "$G/OptiScaler.ini"

# OptiScaler.ini's choices stay
sed -i 's/^Dx11Upscaler=.*/Dx11Upscaler=fsr22/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: a Dx11Upscaler set in OptiScaler.ini stays" 'grep -q "upscalers Dx12=ffx, Dx11=fsr22, Vulkan=ffx_12" "$G/OptiScaler.log"'
sed -i 's/^Dx11Upscaler=.*/Dx11Upscaler=auto/; s/^FfxDx12SRPath=.*/FfxDx12SRPath=C:\\elsewhere\\amd_fidelityfx_upscaler_dx12.dll/' "$G/OptiScaler.ini"
(cd "$G" && timeout 180 wine TestGame.exe >/dev/null 2>&1)
check "FSR DLL: another FfxDx12SRPath in OptiScaler.ini wins" 'grep -q "but FfxDx12SRPath picks" "$G/OptiScaler.log" && ! grep -q "upscalers Dx12=" "$G/OptiScaler.log"'

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
