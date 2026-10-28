#!/usr/bin/env bash
# Tests setup_linux.sh against a made-up Steam library: finding games, installing into the right folder, keeping
# and restoring the game's own files, updating, removing, and a download extracted into the game.
#
#   helixsr/tests/test_setup_linux.sh [PACKAGE_FOLDER]
#
# PACKAGE_FOLDER is an unpacked HelixSR Scaler download; without it a stand-in package is made.
# shellcheck disable=SC2034 # (variables used inside check's eval)
set -uo pipefail

REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fails=0
pass() { printf 'ok   %s\n' "$*"; }
fail() { printf 'FAIL %s\n' "$*"; fails=$((fails + 1)); }
check() { if eval "$2"; then pass "$1"; else fail "$1"; fi; }
snap() { (cd "$1" && find . -type f -exec sha256sum {} + | sort -k2); }

# --- the package ---
P="$W/HelixSR-Scaler"
if [ $# -ge 1 ]; then
    cp -r "$1" "$P"
else
    # as OptiScaler's own download: its FidelityFX and XeSS DLLs in an OptiScaler folder
    mkdir -p "$P/Licenses" "$P/OptiScaler/D3D12_OptiScaler"
    for f in OptiScaler.dll OptiScaler.ini OptiScaler/amd_fidelityfx_loader_dx12.dll \
        OptiScaler/amd_fidelityfx_upscaler_dx12.dll OptiScaler/libxess.dll setup_windows.bat README.txt \
        Licenses/OptiScaler_LICENSE.txt OptiScaler/D3D12_OptiScaler/D3D12Core.dll; do
        echo "package $f" >"$P/$f"
    done
    printf '[Upscalers]\nDx12Upscaler=auto\n[DLSS]\nHelixSR=auto\n[Plugins]\nLoadAsiPlugins=auto\n[FrameGen]\nFGInput=auto\nFGNvngxReplacement=auto\n' >"$P/OptiScaler.ini"
fi
AMDFSR=$(cat "$P/OptiScaler/amd_fidelityfx_upscaler_dx12.dll")
cp "$REPO/helixsr/setup_linux.sh" "$P/setup_linux.sh"
chmod +x "$P/setup_linux.sh"
(cd "$P" && find . -type f | sed 's|^\./||' | sort >HelixSR-Scaler-package.txt)

# --- a home with HelixSR and two Steam libraries ---
H="$W/home"
# a HelixSR 3.x download after its setup: HelixSR/helixsr.dll with the network, HLX-FSR4/ with HLX FSR 4's shaders
HS="$H/Downloads/HelixSR-3.1.0"
mkdir -p "$HS/HelixSR" "$HS/HLX-FSR4"
{ head -c 4096 /dev/urandom; printf 'HXSRNET1'; head -c 24 /dev/zero; } >"$HS/HelixSR/helixsr.dll"
{ printf 'HLX FSR 4 code'; head -c 4096 /dev/urandom; printf 'HLXFSR4D'; head -c 16 /dev/zero; } >"$HS/HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll"
# frame generation as HelixSR's setup leaves it next to the DLL
mkdir -p "$HS/HelixSR/helixsr-fg"
echo "fg provider" >"$HS/HelixSR/helixsr_fg.dll"
for f in nvexec64.dll _nvngx.dll nvngx_dlssg.dll hxfg.cfg; do echo "fg $f" >"$HS/HelixSR/helixsr-fg/$f"; done
mkdir -p "$HS/HelixSR/helixsr-fg/310.9.16" && echo "fg data" >"$HS/HelixSR/helixsr-fg/310.9.16/data.bin"
echo "a log" >"$HS/HelixSR/helixsr-fg/nvexec.log"
# an older, unbuilt download: not picked
mkdir -p "$H/Downloads/HelixSR-3.0.0/HelixSR" && echo "code only" >"$H/Downloads/HelixSR-3.0.0/HelixSR/helixsr.dll"
L1="$H/.local/share/Steam/steamapps" L2="$W/sd"
mkdir -p "$L1" "$L2/steamapps"
printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n}\n' \
    "$H/.local/share/Steam" "$L2" >"$L1/libraryfolders.vdf"
L2="$L2/steamapps"
game() { # game LIBRARY APPID NAME FOLDER
    printf '"AppState"\n{\n\t"appid"\t\t"%s"\n\t"name"\t\t"%s"\n\t"installdir"\t\t"%s"\n}\n' "$2" "$3" "$4" \
        >"$1/appmanifest_$2.acf"
    mkdir -p "$1/common/$4"
}
game "$L1" 10 "Plain Game" "Plain"
echo x >"$L1/common/Plain/Plain.exe"
echo x >"$L1/common/Plain/nvngx_dlss.dll"
game "$L2" 20 "Unreal Game" "UnrealGame"
U="$L2/common/UnrealGame"
mkdir -p "$U/Proj/Binaries/Win64" "$U/Engine/Plugins/DLSS/Binaries/ThirdParty/Win64"
echo x >"$U/Proj.exe"
echo x >"$U/Proj/Binaries/Win64/Proj-Win64-Shipping.exe"
echo "the game's own" >"$U/Proj/Binaries/Win64/amd_fidelityfx_dx12.dll"
echo x >"$U/Engine/Plugins/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll"
game "$L1" 30 "No Upscaler Game" "NoUp"
echo x >"$L1/common/NoUp/NoUp.exe"
game "$L1" 40 "Proton 9.0" "Proton 9.0"
echo x >"$L1/common/Proton 9.0/nvngx_dlss.dll"

export HOME="$H"
run() { "$P/setup_linux.sh" "$@" </dev/null >"$W/out.txt" 2>&1; }

# --- finding games ---
list=$("$P/setup_linux.sh" --list | cut -f1,2)
check "lists games with an upscaler" 'grep -q "^Plain Game	DLSS$" <<<"$list" && grep -q "^Unreal Game	DLSS FSR$" <<<"$list"'
check "skips Proton and games without an upscaler" '! grep -qE "Proton|No Upscaler" <<<"$list"'

# --- install into an Unreal game: Binaries/Win64, not the starter .exe's folder ---
before=$(snap "$U")
run --yes --game "$U"
B="$U/Proj/Binaries/Win64"
check "install succeeds" 'grep -q "installed as" "$W/out.txt"'
check "goes next to the -Shipping.exe" '[ -f "$B/dxgi.dll" ] && [ ! -e "$U/dxgi.dll" ]'
check "OptiScaler.dll becomes dxgi.dll" 'cmp -s "$B/dxgi.dll" "$P/OptiScaler.dll" && [ ! -e "$B/OptiScaler.dll" ]'
check "HLX DLSS copied in as HelixSR/helixsr.dll" 'cmp -s "$B/HelixSR/helixsr.dll" "$HS/HelixSR/helixsr.dll" && [ ! -e "$B/HelixSR-fsrfg" ]'
check "HLX FSR 4 (built) in OptiScaler/, AMD's FSR next to it as .amd.dll" 'cmp -s "$B/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$HS/HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll" && [ "$(cat "$B/OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll")" = "$AMDFSR" ]'
check "OptiScaler.ini: HLX DLSS as the DLSS upscaler, OptiPatcher loaded" 'grep -q "^Dx12Upscaler=dlss" "$B/OptiScaler.ini" && grep -q "^HelixSR=true" "$B/OptiScaler.ini" && grep -q "^LoadAsiPlugins=true" "$B/OptiScaler.ini"'
check "install record: the app's layout line" 'grep -qx "# layout=ngx" "$B/HelixSR-Scaler.files" && grep -qx "name=dxgi.dll" "$B/HelixSR-Scaler.files"'
# the game's own FSR file: kept in the backup when the download has one of that name, else left where it is
if [ -f "$P/amd_fidelityfx_dx12.dll" ]; then GAMEFILE="$B/HelixSR-Scaler-backup/amd_fidelityfx_dx12.dll"; else GAMEFILE="$B/amd_fidelityfx_dx12.dll"; fi
check "game's file kept" 'grep -q "the game'"'"'s own" "$GAMEFILE"'
check "setup files stay out of the game" '[ ! -e "$B/setup_linux.sh" ] && [ ! -e "$B/setup_windows.bat" ] && [ ! -e "$B/README.txt" ]'
check "no launch options for dxgi.dll" 'grep -q "No launch options needed" "$W/out.txt" && ! grep -q "WINEDLLOVERRIDES=" "$W/out.txt"'
check "frame generation copied in" 'cmp -s "$B/HelixSR/helixsr-fg/nvexec64.dll" "$HS/HelixSR/helixsr-fg/nvexec64.dll" && [ -f "$B/HelixSR/helixsr-fg/_nvngx.dll" ] && [ -f "$B/HelixSR/helixsr_fg.dll" ]'
check "frame generation's data files and subfolders too, not its logs" '[ -f "$B/HelixSR/helixsr-fg/hxfg.cfg" ] && [ -f "$B/HelixSR/helixsr-fg/310.9.16/data.bin" ] && [ ! -e "$B/HelixSR/helixsr-fg/nvexec.log" ]'
if grep -q "^FGInput=" "$B/OptiScaler.ini" 2>/dev/null; then
    check "--yes switches HelixSR frame generation on (FGInput=nvngxfg, FGNvngxReplacement=HelixSR)" 'grep -q "^FGInput=nvngxfg" "$B/OptiScaler.ini" && grep -q "^FGNvngxReplacement=HelixSR" "$B/OptiScaler.ini" && grep -q "Frame generation included and on" "$W/out.txt"'
    check "no [HelixSR] FrameGeneration / FGPresent lines" '! grep -q "^FrameGeneration=\|^FGPresent=" "$B/OptiScaler.ini"'
fi
list=$("$P/setup_linux.sh" --list | cut -f1,2)
check "own files don't count as the game's upscalers" 'grep -q "^Unreal Game	DLSS FSR$" <<<"$list"'

# --- update keeps the original backup and the settings ---
echo "WaveSize=64" >>"$B/OptiScaler.ini"
run --yes --game "$U"
check "update keeps OptiScaler.ini" 'grep -q "^WaveSize=64" "$B/OptiScaler.ini"'
# settings from before: [HelixSR] FrameGeneration=on becomes the two [FrameGen] lines; a choice made stays
if grep -q "^FGInput=" "$B/OptiScaler.ini"; then
    sed -i 's/^FGInput=.*/FGInput=auto/; s/^FGNvngxReplacement=.*/FGNvngxReplacement=auto/' "$B/OptiScaler.ini"
    printf 'FrameGeneration=on\nFGPresent=auto\n' >>"$B/OptiScaler.ini"
    run --yes --game "$U"
    check "update: old FrameGeneration=on -> FGInput=nvngxfg, FGNvngxReplacement=HelixSR" 'grep -q "^FGInput=nvngxfg" "$B/OptiScaler.ini" && grep -q "^FGNvngxReplacement=HelixSR" "$B/OptiScaler.ini" && ! grep -q "^FrameGeneration=\|^FGPresent=" "$B/OptiScaler.ini"'
    sed -i 's/^FGInput=.*/FGInput=nofg/' "$B/OptiScaler.ini"
    run --yes --game "$U"
    check "update: a frame-generation choice in OptiScaler.ini stays" 'grep -q "^FGInput=nofg" "$B/OptiScaler.ini" && grep -q "Frame generation included, off" "$W/out.txt"'
fi
check "update keeps the game's file" 'grep -q "the game'"'"'s own" "$GAMEFILE"'
check "update says so" 'grep -q "updating" "$W/out.txt"'

# --- remove with the copy left in the game: everything as before ---
F="$B/HelixSR"
touch "$B/OptiScaler.log" "$F/helixsr.log" "$F/helixsr.ini" "$F/helixsr-fg/nvexec.log" "$B/OptiScaler/HLX_FSR.log"
"$B/HelixSR-Scaler-remove.sh" --yes </dev/null >"$W/out.txt" 2>&1
check "remove restores the game exactly" '[ "$before" = "$(snap "$U")" ]'
check "remove leaves no empty folders" '[ -z "$(find "$B" -mindepth 1 -type d -empty)" ]'
run --yes --remove --game "$U"
check "remove when not installed says so" 'grep -q "isn'"'"'t installed" "$W/out.txt"'

# --- an install of HelixSR 2.0 (HelixSR Scaler 1.1.0: HelixSR-fsrfg/): updated to HelixSR 3.x, the old folder taken out ---
mkdir -p "$B/HelixSR-fsrfg/helixsr-fg"
echo "HelixSR 2.0" >"$B/HelixSR-fsrfg/amd_fidelityfx_dx12.dll"
cp "$HS/HelixSR/helixsr-fg/"*.dll "$B/HelixSR-fsrfg/helixsr-fg/"
printf 'name=dxgi.dll\ndxgi.dll\nHelixSR-fsrfg/amd_fidelityfx_dx12.dll\nHelixSR-fsrfg/helixsr-fg/nvexec64.dll\nHelixSR-fsrfg/helixsr-fg/_nvngx.dll\nHelixSR-fsrfg/helixsr-fg/nvngx_dlssg.dll\n' >"$B/HelixSR-Scaler.files"
cp "$P/OptiScaler.dll" "$B/dxgi.dll"
run --yes --game "$U"
check "HelixSR 2.0 install: updated to HelixSR/, HelixSR-fsrfg taken out" 'cmp -s "$B/HelixSR/helixsr.dll" "$HS/HelixSR/helixsr.dll" && [ -f "$B/HelixSR/helixsr-fg/nvexec64.dll" ] && [ ! -e "$B/HelixSR-fsrfg" ]'
"$B/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1
check "older install updated and removed: the game as before" '[ "$before" = "$(snap "$U")" ]'

# --- picking from the list, other names ---
printf '1\n' | "$P/setup_linux.sh" --yes --name winmm.dll >"$W/out.txt" 2>&1
check "pick by number, --name" '[ -f "$L1/common/Plain/winmm.dll" ] && grep -q "winmm=n,b" "$W/out.txt"'
printf '\n' | "$P/setup_linux.sh" >"$W/out.txt" 2>&1
check "empty answer changes nothing" 'grep -q "Nothing changed" "$W/out.txt"'
"$L1/common/Plain/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1

# --- no HelixSR anywhere ---
mv "$HS" "$W/hidden"
printf '%s\n' "$W/out.txt" | "$P/setup_linux.sh" --game "$L1/common/Plain" >"$W/out.txt" 2>&1
check "asks for HelixSR and refuses a wrong file" 'grep -q "HelixSR wasn'"'"'t found" "$W/out.txt" && grep -q "network built in" "$W/out.txt"'
check "nothing installed without HelixSR" '[ ! -e "$L1/common/Plain/dxgi.dll" ]'
mv "$W/hidden" "$HS"

# --- HLX FSR 4 not built (HelixSR's setup stopped before it): AMD's FSR stays under its own name ---
mv "$HS/HLX-FSR4" "$W/hidden-hlx"
run --yes --game "$L1/common/Plain"
check "HLX FSR 4 not built: AMD's FSR stays, no .amd.dll" '[ "$(cat "$L1/common/Plain/OptiScaler/amd_fidelityfx_upscaler_dx12.dll")" = "$AMDFSR" ] && [ ! -e "$L1/common/Plain/OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll" ] && grep -q "HLX FSR 4 isn'"'"'t built" "$W/out.txt"'
"$L1/common/Plain/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1
mv "$W/hidden-hlx" "$HS/HLX-FSR4"

# --- an unbuilt HLX FSR 4 put over AMD's in the HelixSR Scaler folder by hand: never installed as AMD's ---
cp "$P/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$W/amd-keep"
printf 'HLX FSR 4 code only' >"$P/OptiScaler/amd_fidelityfx_upscaler_dx12.dll"
run --yes --game "$L1/common/Plain"
check "unbuilt HLX FSR 4 in the download: the built one goes in, no fake AMD file" 'cmp -s "$L1/common/Plain/OptiScaler/amd_fidelityfx_upscaler_dx12.dll" "$HS/HLX-FSR4/amd_fidelityfx_upscaler_dx12.dll" && [ ! -e "$L1/common/Plain/OptiScaler/amd_fidelityfx_upscaler_dx12.amd.dll" ]'
"$L1/common/Plain/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1
cp "$W/amd-keep" "$P/OptiScaler/amd_fidelityfx_upscaler_dx12.dll"

# --- download extracted into the game, as with OptiScaler ---
G="$L1/common/Plain"
orig=$(cd "$G" && ls -A | sort)
cp -r "$P/." "$G/"
(cd "$G" && ./setup_linux.sh --yes </dev/null >"$W/out.txt" 2>&1)
check "in-game install works" '[ -f "$G/dxgi.dll" ] && [ ! -e "$G/OptiScaler.dll" ] && [ -f "$G/HelixSR/helixsr.dll" ]'
check "in-game install cleans up the download" '[ ! -e "$G/setup_linux.sh" ] && [ ! -e "$G/HelixSR-Scaler-package.txt" ]'
check "in-game install backs nothing up" '[ ! -e "$G/HelixSR-Scaler-backup" ]'
(cd "$G" && ./HelixSR-Scaler-remove.sh --yes </dev/null >/dev/null 2>&1)
check "in-game remove leaves only the game" '[ "$orig" = "$(cd "$G" && ls -A | sort)" ]'

echo
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
