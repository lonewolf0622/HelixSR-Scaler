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
fi
cp "$REPO/helixsr/setup_linux.sh" "$P/setup_linux.sh"
chmod +x "$P/setup_linux.sh"
(cd "$P" && find . -type f | sed 's|^\./||' | sort >HelixSR-Scaler-package.txt)

# --- a home with HelixSR and two Steam libraries ---
H="$W/home"
mkdir -p "$H/Downloads/HelixSR-1.9.0"
{ head -c 4096 /dev/urandom; printf 'HXSRNET1'; head -c 24 /dev/zero; } >"$H/Downloads/HelixSR-1.9.0/amd_fidelityfx_dx12.dll"
# frame generation as HelixSR's setup leaves it next to the DLL
mkdir -p "$H/Downloads/HelixSR-1.9.0/helixsr-fg"
for f in nvexec64.dll _nvngx.dll nvngx_dlssg.dll hxfg.cfg; do echo "fg $f" >"$H/Downloads/HelixSR-1.9.0/helixsr-fg/$f"; done
mkdir -p "$H/Downloads/HelixSR-1.9.0/helixsr-fg/310.9.16" && echo "fg data" >"$H/Downloads/HelixSR-1.9.0/helixsr-fg/310.9.16/data.bin"
echo "a log" >"$H/Downloads/HelixSR-1.9.0/helixsr-fg/nvexec.log"
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
check "HelixSR copied in as HelixSR-fsrfg/amd_fidelityfx_dx12.dll" 'cmp -s "$B/HelixSR-fsrfg/amd_fidelityfx_dx12.dll" "$H/Downloads/HelixSR-1.9.0/amd_fidelityfx_dx12.dll" && [ ! -e "$B/HelixSR" ]'
# the game's own FSR file: kept in the backup when the download has one of that name, else left where it is
if [ -f "$P/amd_fidelityfx_dx12.dll" ]; then GAMEFILE="$B/HelixSR-Scaler-backup/amd_fidelityfx_dx12.dll"; else GAMEFILE="$B/amd_fidelityfx_dx12.dll"; fi
check "game's file kept" 'grep -q "the game'"'"'s own" "$GAMEFILE"'
check "setup files stay out of the game" '[ ! -e "$B/setup_linux.sh" ] && [ ! -e "$B/setup_windows.bat" ] && [ ! -e "$B/README.txt" ]'
check "no launch options for dxgi.dll" 'grep -q "No launch options needed" "$W/out.txt" && ! grep -q "WINEDLLOVERRIDES=" "$W/out.txt"'
check "frame generation copied in" 'cmp -s "$B/HelixSR-fsrfg/helixsr-fg/nvexec64.dll" "$H/Downloads/HelixSR-1.9.0/helixsr-fg/nvexec64.dll" && [ -f "$B/HelixSR-fsrfg/helixsr-fg/_nvngx.dll" ]'
check "frame generation's data files and subfolders too, not its logs" '[ -f "$B/HelixSR-fsrfg/helixsr-fg/hxfg.cfg" ] && [ -f "$B/HelixSR-fsrfg/helixsr-fg/310.9.16/data.bin" ] && [ ! -e "$B/HelixSR-fsrfg/helixsr-fg/nvexec.log" ]'
if [ -f "$B/OptiScaler.ini" ] && grep -q "^FrameGeneration=" "$B/OptiScaler.ini"; then
    check "--yes switches frame generation on" 'grep -q "^FrameGeneration=on" "$B/OptiScaler.ini" && grep -q "Frame generation included and on" "$W/out.txt"'
fi
list=$("$P/setup_linux.sh" --list | cut -f1,2)
check "own files don't count as the game's upscalers" 'grep -q "^Unreal Game	DLSS FSR$" <<<"$list"'

# --- update keeps the original backup and the settings ---
echo "WaveSize=64" >>"$B/OptiScaler.ini"
run --yes --game "$U"
check "update keeps OptiScaler.ini" 'grep -q "^WaveSize=64" "$B/OptiScaler.ini"'
check "update keeps the game's file" 'grep -q "the game'"'"'s own" "$GAMEFILE"'
check "update says so" 'grep -q "updating" "$W/out.txt"'

# --- remove with the copy left in the game: everything as before ---
F="$B/HelixSR-fsrfg"
touch "$B/OptiScaler.log" "$F/helixsr.log" "$F/helixsr.ini" "$F/helixsr-fg/nvexec.log"
mkdir -p "$F/helixsr-fg.off" && touch "$F/helixsr-fg.off/nvexec64.dll" # frame generation switched off
cp "$F/amd_fidelityfx_dx12.dll" "$F/amd_fidelityfx_upscaler_dx12.amd.dll" # AMD's FSR, put there by HelixSR Scaler
"$B/HelixSR-Scaler-remove.sh" --yes </dev/null >"$W/out.txt" 2>&1
check "remove restores the game exactly" '[ "$before" = "$(snap "$U")" ]'
check "remove leaves no empty folders" '[ -z "$(find "$B" -mindepth 1 -type d -empty)" ]'
run --yes --remove --game "$U"
check "remove when not installed says so" 'grep -q "isn'"'"'t installed" "$W/out.txt"'

# --- an install from before 1.1.0 (HelixSR/helixsr.dll): updated to HelixSR-fsrfg, the old folder taken out ---
mkdir -p "$B/HelixSR/helixsr-fg"
cp "$H/Downloads/HelixSR-1.9.0/amd_fidelityfx_dx12.dll" "$B/HelixSR/helixsr.dll"
cp "$H/Downloads/HelixSR-1.9.0/helixsr-fg/"*.dll "$B/HelixSR/helixsr-fg/"
printf 'name=dxgi.dll\ndxgi.dll\nHelixSR/helixsr.dll\nHelixSR/helixsr-fg/nvexec64.dll\nHelixSR/helixsr-fg/_nvngx.dll\nHelixSR/helixsr-fg/nvngx_dlssg.dll\n' >"$B/HelixSR-Scaler.files"
cp "$P/OptiScaler.dll" "$B/dxgi.dll"
mv "$H/Downloads/HelixSR-1.9.0" "$W/hidden"
run --yes --game "$U"
mv "$W/hidden" "$H/Downloads/HelixSR-1.9.0"
check "older install: HelixSR moves to HelixSR-fsrfg" 'cmp -s "$B/HelixSR-fsrfg/amd_fidelityfx_dx12.dll" "$H/Downloads/HelixSR-1.9.0/amd_fidelityfx_dx12.dll" && [ -f "$B/HelixSR-fsrfg/helixsr-fg/nvexec64.dll" ] && [ ! -e "$B/HelixSR" ]'
"$B/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1
check "older install updated and removed: the game as before" '[ "$before" = "$(snap "$U")" ]'

# --- picking from the list, other names ---
printf '1\n' | "$P/setup_linux.sh" --yes --name winmm.dll >"$W/out.txt" 2>&1
check "pick by number, --name" '[ -f "$L1/common/Plain/winmm.dll" ] && grep -q "winmm=n,b" "$W/out.txt"'
printf '\n' | "$P/setup_linux.sh" >"$W/out.txt" 2>&1
check "empty answer changes nothing" 'grep -q "Nothing changed" "$W/out.txt"'
"$L1/common/Plain/HelixSR-Scaler-remove.sh" --yes </dev/null >/dev/null 2>&1

# --- no HelixSR anywhere ---
mv "$H/Downloads/HelixSR-1.9.0" "$W/hidden"
printf '%s\n' "$W/out.txt" | "$P/setup_linux.sh" --game "$L1/common/Plain" >"$W/out.txt" 2>&1
check "asks for HelixSR and refuses a wrong file" 'grep -q "HelixSR wasn'"'"'t found" "$W/out.txt" && grep -q "network built in" "$W/out.txt"'
check "nothing installed without HelixSR" '[ ! -e "$L1/common/Plain/dxgi.dll" ]'
mv "$W/hidden" "$H/Downloads/HelixSR-1.9.0"

# --- download extracted into the game, as with OptiScaler ---
G="$L1/common/Plain"
orig=$(cd "$G" && ls -A | sort)
cp -r "$P/." "$G/"
(cd "$G" && ./setup_linux.sh --yes </dev/null >"$W/out.txt" 2>&1)
check "in-game install works" '[ -f "$G/dxgi.dll" ] && [ ! -e "$G/OptiScaler.dll" ] && [ -f "$G/HelixSR-fsrfg/amd_fidelityfx_dx12.dll" ]'
check "in-game install cleans up the download" '[ ! -e "$G/setup_linux.sh" ] && [ ! -e "$G/HelixSR-Scaler-package.txt" ]'
check "in-game install backs nothing up" '[ ! -e "$G/HelixSR-Scaler-backup" ]'
(cd "$G" && ./HelixSR-Scaler-remove.sh --yes </dev/null >/dev/null 2>&1)
check "in-game remove leaves only the game" '[ "$orig" = "$(cd "$G" && ls -A | sort)" ]'

echo
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
