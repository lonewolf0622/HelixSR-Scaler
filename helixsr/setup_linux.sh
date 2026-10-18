#!/usr/bin/env bash
# HelixSR Scaler setup for Linux and Steam Deck.
#
# Puts HelixSR Scaler and your HelixSR into a game, or takes them out again. Games run it through Proton or Wine,
# the same files as on Windows. In the game: OptiScaler.dll as dxgi.dll (or --name), HelixSR's DLL in
# HelixSR-fsrfg/amd_fidelityfx_dx12.dll and its frame generation in HelixSR-fsrfg/helixsr-fg.
#
#   ./setup_linux.sh                      asks which game, then installs
#   ./setup_linux.sh --game FOLDER        installs into that game (the folder with its .exe, or the game's folder)
#   ./setup_linux.sh --remove [--game FOLDER]
#   ./setup_linux.sh --list               lists the games it finds
#
# More options:
#   --helixsr FILE   HelixSR's amd_fidelityfx_dx12.dll (after its setup built the network); found by itself
#                    when it's in your Downloads, next to this folder or in the HelixSR app
#   --name NAME      what OptiScaler.dll is called in the game: dxgi.dll (default), winmm.dll, version.dll,
#                    dbghelp.dll, d3d12.dll, wininet.dll or winhttp.dll
#   --yes            answers yes to every question
#
# A copy of this script is left in the game as HelixSR-Scaler-remove.sh: run it to take HelixSR Scaler out. Files
# the game had before are kept in HelixSR-Scaler-backup and put back then.

set -uo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SELF="$HERE/$(basename "${BASH_SOURCE[0]}")"
MANIFEST="HelixSR-Scaler.files"
BACKUP="HelixSR-Scaler-backup"
REMOVER="HelixSR-Scaler-remove.sh"
NAMES="dxgi.dll winmm.dll version.dll dbghelp.dll d3d12.dll wininet.dll winhttp.dll"
# Package files that stay in the download
NOT_COPIED="setup_linux.sh setup_windows.bat README.txt README-Linux.txt HelixSR-Scaler-package.txt"
# The download's files, one per line (made when the download is packed); without it, everything next to this script
PACKAGE_LIST="HelixSR-Scaler-package.txt"
# HelixSR's folder next to the game's .exe: its FidelityFX DLL (amd_fidelityfx_dx12.dll) and frame generation
FSRFG="HelixSR-fsrfg"

if [ -t 1 ]; then
    B=$'\e[1m' D=$'\e[2m' T=$'\e[38;2;45;212;191m' V=$'\e[38;2;167;139;250m' A=$'\e[38;2;245;184;75m'
    R=$'\e[38;2;242;122;122m' N=$'\e[0m'
else
    B="" D="" T="" V="" A="" R="" N=""
fi

say() { printf '%s\n' "$*"; }
ok() { printf '  %s✓%s %s\n' "$T" "$N" "$*"; }
warn() { printf '  %s!%s %s\n' "$A" "$N" "$*"; }
die() {
    printf '\n  %s✗ %s%s\n\n' "$R" "$*" "$N" >&2
    pause
    exit 1
}
pause() { [ "$ASSUME_YES" = 1 ] || [ ! -t 0 ] || read -r -p "  Press Enter to close..." _ || true; }
ask() { # ask "question" -> 0 for yes (default yes)
    [ "$ASSUME_YES" = 1 ] && return 0
    local r
    read -r -p "  $1 [Y/n] " r || r=n
    case "$r" in '' | y | Y | yes | YES) return 0 ;; *) return 1 ;; esac
}
title() {
    say ""
    say "  ${T}${B}HelixSR Scaler${N} ${D}setup for Linux and Steam Deck${N}"
    say "  ${D}$(printf '%.0s─' {1..44})${N}"
    say ""
}

# --- arguments ---
GAME="" HELIX="" NAME="" ACTION=install ASSUME_YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAME="${2:-}"; shift ;;
        --game=*) GAME="${1#*=}" ;;
        --helixsr) HELIX="${2:-}"; shift ;;
        --helixsr=*) HELIX="${1#*=}" ;;
        --name) NAME="${2:-}"; shift ;;
        --name=*) NAME="${1#*=}" ;;
        --filename=*) NAME="${1#*=}" ;; # OptiScaler's own option
        --remove) ACTION=remove ;;
        --list) ACTION=list ;;
        --yes | -y) ASSUME_YES=1 ;;
        -h | --help)
            sed -n '2,20p' "$SELF" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *) [ -d "$1" ] && GAME="$1" || { say "Unknown option: $1 (--help lists them)"; exit 1; } ;;
    esac
    shift
done

# The copy left in a game removes HelixSR Scaler from that game
if [ "$(basename "$SELF")" = "$REMOVER" ]; then
    ACTION=remove
    [ -n "$GAME" ] || GAME="$HERE"
fi

# --- finding things ---

# HelixSR's DLL with the network built into it ends with a 32-byte trailer that starts with HXSRNET1
is_built_helixsr() { [ -f "$1" ] && [ "$(tail -c 32 "$1" 2>/dev/null | head -c 8 | tr -d '\0')" = HXSRNET1 ]; }

find_helixsr() {
    local c data="${XDG_DATA_HOME:-$HOME/.local/share}"
    for c in "$HERE/$FSRFG/amd_fidelityfx_dx12.dll" "$HERE/HelixSR/helixsr.dll" "$HERE/amd_fidelityfx_dx12.dll" \
        "$HERE"/../HelixSR*/amd_fidelityfx_dx12.dll \
        "$data"/HelixSR-Desktop/releases/*/HelixSR*/amd_fidelityfx_dx12.dll \
        "$data"/HelixSR-Desktop/releases/*/amd_fidelityfx_dx12.dll \
        "$HOME"/Downloads/HelixSR*/amd_fidelityfx_dx12.dll "$HOME"/Downloads/*/HelixSR*/amd_fidelityfx_dx12.dll \
        "$HOME"/HelixSR*/amd_fidelityfx_dx12.dll "$HOME"/Desktop/HelixSR*/amd_fidelityfx_dx12.dll; do
        if is_built_helixsr "$c"; then
            realpath "$c"
            return 0
        fi
    done
    return 1
}

steam_libraries() { # every steamapps folder, once
    local roots=("$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam")
    local r p
    {
        for r in "${roots[@]}"; do
            [ -d "$r/steamapps" ] && realpath "$r/steamapps"
            [ -f "$r/steamapps/libraryfolders.vdf" ] || continue
            grep -E '^\s*"path"' "$r/steamapps/libraryfolders.vdf" | sed -E 's/^\s*"path"\s*"(.*)"\s*$/\1/; s/\\\\/\\/g' |
                while read -r p; do [ -d "$p/steamapps" ] && realpath "$p/steamapps"; done
        done
    } 2>/dev/null | awk '!seen[$0]++'
}

# The game's own .exe folder: Unreal's Binaries/Win64 first, then the biggest .exe that isn't a launcher or helper
exe_folder() {
    local dir="$1" exe
    exe=$(find "$dir" -maxdepth 6 -type f -iname '*-Win64-Shipping.exe' -not -ipath '*/Engine/*' 2>/dev/null | head -n 1)
    if [ -z "$exe" ]; then
        exe=$(find "$dir" -maxdepth 4 -type f -iname '*.exe' \
            -not -ipath '*redist*' -not -ipath '*/Engine/*' -not -ipath '*installer*' -not -ipath '*/__Installer/*' \
            -not -ipath '*EasyAntiCheat*' -not -ipath '*BattlEye*' -not -ipath '*/support/*' \
            -not -iname 'unins*' -not -iname '*crash*' -not -iname '*launcher*' -not -iname 'dxsetup*' \
            -not -iname 'vc_redist*' -not -iname 'UnityCrashHandler*' -not -iname '*report*' -not -iname 'setup*' \
            -printf '%s\t%p\n' 2>/dev/null | sort -rn | head -n 1 | cut -f2-)
    fi
    [ -n "$exe" ] && dirname "$exe"
}

# What the game offers that HelixSR Scaler can take over (its own files don't count, nor the game's in the backup)
upscalers_in() {
    local dir="$1" out=() f all
    all=$(find "$dir" -maxdepth 7 -type f \( -iname 'nvngx_dlss.dll' -o -iname 'amd_fidelityfx_*dx12*.dll' \
        -o -iname 'ffx_fsr2_api_*x64*.dll' -o -iname 'libxess.dll' \) -not -path "*/$BACKUP/*" 2>/dev/null |
        while IFS= read -r f; do
            # the install record next to the .exe lists HelixSR Scaler's files, also those in its subfolders
            # (OptiScaler\amd_fidelityfx_*.dll, HelixSR\...): they don't count, unless they replaced the game's own
            local d rel n
            d=$(dirname "$f") rel=$(basename "$f")
            for n in 1 2 3; do
                [ -f "$d/$MANIFEST" ] && break
                rel="$(basename "$d")/$rel" d=$(dirname "$d")
            done
            if [ -f "$d/$MANIFEST" ] && grep -qxF "$rel" "$d/$MANIFEST"; then
                [ -f "$d/$BACKUP/$rel" ] && basename "$f"
            else
                basename "$f"
            fi
        done)
    grep -qix 'nvngx_dlss.dll' <<<"$all" && out+=(DLSS)
    grep -qiE '^(amd_fidelityfx_.*dx12.*|ffx_fsr2_api_.*x64.*)\.dll$' <<<"$all" && out+=(FSR)
    grep -qix 'libxess.dll' <<<"$all" && out+=(XeSS)
    echo "${out[*]:-}"
}

# The game's name: Steam's, else its folder's
title_for() {
    local dir="$1" lib inst acf name
    if [[ "$dir" == */steamapps/common/* ]]; then
        lib="${dir%%/steamapps/common/*}/steamapps"
        inst="${dir#*/steamapps/common/}" inst="${inst%%/*}"
        for acf in "$lib"/appmanifest_*.acf; do
            [ -f "$acf" ] || continue
            if grep -qE "^\s*\"installdir\"\s*\"$(printf '%s' "$inst" | sed 's/[]\.*^$[]/\\&/g')\"" "$acf"; then
                name=$(sed -nE 's/^\s*"name"\s*"(.*)"\s*$/\1/p' "$acf" | head -n 1)
                [ -n "$name" ] && { echo "$name"; return; }
            fi
        done
        echo "$inst"
        return
    fi
    case "$dir" in */Binaries/Win64) basename "$(dirname "$(dirname "$(dirname "$dir")")")" ;; *) basename "$dir" ;; esac
}

has_anticheat() {
    find "$1" -maxdepth 5 \( -iname 'EasyAntiCheat*' -o -iname 'BattlEye*' -o -iname 'start_protected_game.exe' \) \
        -print -quit 2>/dev/null | grep -q .
}

GAME_NAMES=() GAME_DIRS=() GAME_UPS=()
find_games() {
    local name dir ups
    while IFS=$'\t' read -r name ups dir; do
        GAME_NAMES+=("$name") GAME_DIRS+=("$dir") GAME_UPS+=("$ups")
    done < <(steam_games | sort -f -t $'\t' -k1,1)
}
steam_games() { # name, upscalers, folder; tab separated
    local lib acf name dir ups
    while read -r lib; do
        for acf in "$lib"/appmanifest_*.acf; do
            [ -f "$acf" ] || continue
            name=$(sed -nE 's/^\s*"name"\s*"(.*)"\s*$/\1/p' "$acf" | head -n 1)
            dir=$(sed -nE 's/^\s*"installdir"\s*"(.*)"\s*$/\1/p' "$acf" | head -n 1)
            [ -n "$dir" ] && [ -d "$lib/common/$dir" ] || continue
            case "$name" in Proton* | "Steam Linux Runtime"* | Steamworks* | "Steam Audio"*) continue ;; esac
            ups=$(upscalers_in "$lib/common/$dir")
            [ -n "$ups" ] || continue
            printf '%s\t%s\t%s\n' "$name" "$ups" "$lib/common/$dir"
        done
    done < <(steam_libraries)
}

installed_in() { [ -f "$1/$MANIFEST" ]; }

# --- remove ---

remove_from() { # remove_from EXE_FOLDER [quiet]
    local dir="$1" quiet="${2:-}" f
    installed_in "$dir" || { [ -n "$quiet" ] && return 0; die "HelixSR Scaler isn't installed in $dir"; }
    while IFS= read -r f; do
        case "$f" in '' | '#'* | name=*) continue ;; esac
        rm -f "$dir/$f"
    done <"$dir/$MANIFEST"
    # what OptiScaler and HelixSR make while the game runs (HelixSR/ is where versions before 1.1.0 put HelixSR)
    local h
    rm -f "$dir/OptiScaler.log" "$dir/HelixSR/helixsr_upscaler.dll" "$dir/$FSRFG/amd_fidelityfx_upscaler_dx12.amd.dll"
    for h in "$dir/HelixSR" "$dir/$FSRFG"; do
        rm -f "$h/helixsr.log" "$h/helixsr.ini" "$h/helixsr-fg/nvexec.log" "$h/helixsr-fg/hxfg.log"
        # frame generation off in OptiScaler.ini ([HelixSR] FrameGeneration): HelixSR Scaler parked its folder
        rm -rf "${h:?}/helixsr-fg.off"
    done
    if [ -d "$dir/$BACKUP" ]; then
        cp -a "$dir/$BACKUP/." "$dir/" && rm -rf "${dir:?}/$BACKUP"
    fi
    rm -f "$dir/$MANIFEST" "$dir/$REMOVER"
    # folders HelixSR Scaler brought, if nothing else is in them now
    find "$dir/HelixSR" "$dir/$FSRFG" "$dir/OptiScaler" "$dir/D3D12_Optiscaler" "$dir/Licenses" "$dir/DlssOverrides" -depth -type d \
        -empty -delete 2>/dev/null
    [ -n "$quiet" ] || ok "HelixSR Scaler removed; the game's own files are back"
}

# --- choosing a game ---

pick_game() {
    [ -n "$GAME" ] && return 0
    # Extracted into the game's folder, as with OptiScaler: that's the game
    if compgen -G "$HERE/*.[eE][xX][eE]" >/dev/null; then
        GAME="$HERE"
        return 0
    fi
    find_games
    if [ ${#GAME_NAMES[@]} -gt 0 ]; then
        say "  ${B}Games with DLSS, FSR or XeSS${N}"
        say ""
        local i mark
        for i in "${!GAME_NAMES[@]}"; do
            mark=""
            local d
            d=$(exe_folder "${GAME_DIRS[$i]}")
            [ -n "$d" ] && installed_in "$d" && mark=" ${T}● installed${N}"
            printf '   %s%2d%s  %-34s %s%s%s%s\n' "$B" $((i + 1)) "$N" "${GAME_NAMES[$i]:0:34}" "$V" "${GAME_UPS[$i]}" "$N" "$mark"
        done
        say ""
        say "  ${D}Or drag a game's folder into this window (Heroic, Lutris, other stores).${N}"
    else
        say "  No Steam games with DLSS, FSR or XeSS found."
        say "  ${D}Drag the game's folder into this window, or type its path.${N}"
    fi
    say ""
    local r
    read -r -p "  Game: " r || exit 1
    say ""
    r="${r#\'}" r="${r%\'}" r="${r#\"}" r="${r%\"}" # dragged folders arrive in quotes
    r="${r%"${r##*[![:space:]]}"}"
    if [ -z "$r" ]; then
        say "  Nothing changed."
        exit 0
    fi
    if [[ "$r" =~ ^[0-9]+$ ]] && [ "$r" -ge 1 ] && [ "$r" -le ${#GAME_NAMES[@]} ]; then
        GAME="${GAME_DIRS[$((r - 1))]}" GAME_TITLE="${GAME_NAMES[$((r - 1))]}"
    elif [ -n "$r" ] && [ -d "${r/#\~/$HOME}" ]; then
        GAME="${r/#\~/$HOME}"
    else
        die "That isn't a number from the list or a folder"
    fi
}

copy_to_clipboard() {
    if [ -n "${WAYLAND_DISPLAY:-}" ] && command -v wl-copy >/dev/null; then printf '%s' "$1" | wl-copy
    elif command -v xclip >/dev/null && [ -n "${DISPLAY:-}" ]; then printf '%s' "$1" | xclip -selection clipboard
    elif command -v xsel >/dev/null && [ -n "${DISPLAY:-}" ]; then printf '%s' "$1" | xsel --clipboard --input
    else return 1; fi
}

# ======================================================================================================================

[ -t 1 ] && [ "$ASSUME_YES" = 0 ] && clear
title

if [ "$ACTION" = list ]; then
    find_games
    for i in "${!GAME_NAMES[@]}"; do printf '%s\t%s\t%s\n' "${GAME_NAMES[$i]}" "${GAME_UPS[$i]}" "${GAME_DIRS[$i]}"; done
    exit 0
fi

GAME_TITLE=""
pick_game
GAME=$(realpath "$GAME") || die "Can't open $GAME"
# A game's main folder or its .exe folder both work. Unreal games have a small starter .exe in the main folder;
# the real one (and HelixSR Scaler) goes in Binaries/Win64.
SHIPPING=$(find "$GAME" -maxdepth 6 -type f -iname '*-Win64-Shipping.exe' -not -ipath '*/Engine/*' 2>/dev/null | head -n 1)
if installed_in "$GAME"; then
    TARGET="$GAME"
elif [ -n "$SHIPPING" ]; then
    TARGET=$(dirname "$SHIPPING")
elif compgen -G "$GAME/*.[eE][xX][eE]" >/dev/null; then
    TARGET="$GAME"
else
    TARGET=$(exe_folder "$GAME") || true
    [ -n "$TARGET" ] || die "No game .exe found in $GAME"
fi
[ -n "$GAME_TITLE" ] || GAME_TITLE=$(title_for "$GAME")
[ -w "$TARGET" ] || die "Can't write to $TARGET"

if [ "$ACTION" = remove ]; then
    say "  Removing HelixSR Scaler from ${B}$GAME_TITLE${N}"
    say "  ${D}$TARGET${N}"
    say ""
    installed_in "$TARGET" || die "HelixSR Scaler isn't installed in this game"
    ask "Remove it?" || exit 0
    oldname=$(sed -n 's/^name=//p' "$TARGET/$MANIFEST" | head -n 1)
    remove_from "$TARGET"
    say ""
    [ "${oldname:-dxgi.dll}" = dxgi.dll ] ||
        say "  ${D}You can take WINEDLLOVERRIDES out of the game's launch options now.${N}"
    say ""
    pause
    exit 0
fi

# --- install ---
[ -f "$HERE/OptiScaler.dll" ] || die "OptiScaler.dll isn't next to this script. Extract the whole HelixSR Scaler download."

say "  ${B}$GAME_TITLE${N}"
say "  ${D}$TARGET${N}"
say ""

if has_anticheat "$GAME"; then
    warn "This game has anti-cheat. Don't use HelixSR Scaler in online games: you can get banned."
    ask "Install anyway (for offline play)?" || exit 0
fi

# HelixSR
if [ -n "$HELIX" ]; then
    is_built_helixsr "$HELIX" || die "$HELIX isn't HelixSR's DLL with the network built in (run HelixSR's setup first)"
    HELIX=$(realpath "$HELIX")
elif HELIX=$(find_helixsr); then
    :
elif installed_in "$TARGET" && is_built_helixsr "$TARGET/$FSRFG/amd_fidelityfx_dx12.dll"; then
    HELIX="$TARGET/$FSRFG/amd_fidelityfx_dx12.dll" # reinstall keeps the HelixSR already in the game
elif installed_in "$TARGET" && is_built_helixsr "$TARGET/HelixSR/helixsr.dll"; then
    HELIX="$TARGET/HelixSR/helixsr.dll" # an install from before 1.1.0
else
    warn "HelixSR wasn't found. Run helixsr-setup.sh from the HelixSR download first (it builds the"
    warn "network), or use the HelixSR app. Then drag its amd_fidelityfx_dx12.dll in here."
    read -r -p "  HelixSR DLL: " HELIX || exit 1
    HELIX="${HELIX#\'}" HELIX="${HELIX%\'}" HELIX="${HELIX#\"}" HELIX="${HELIX%\"}"
    HELIX="${HELIX%"${HELIX##*[![:space:]]}"}"
    is_built_helixsr "$HELIX" || die "That isn't HelixSR's DLL with the network built in"
    HELIX=$(realpath "$HELIX")
fi
ok "HelixSR: ${D}$HELIX${N}"

# Copy HelixSR out of the game before removing an older install, which would delete it
TMP_HELIX=$(mktemp)
TMP_FG=$(mktemp -d)
trap 'rm -f "$TMP_HELIX"; rm -rf "$TMP_FG"' EXIT
cp -f "$HELIX" "$TMP_HELIX"
# Frame generation: the helixsr-fg folder HelixSR's setup made next to its DLL (a reinstall: the game's own copy)
# All of its files (data files and subfolders too), not its logs
for d in "$(dirname "$HELIX")/helixsr-fg" "$(dirname "$HELIX")/helixsr-fg.off" "$(dirname "$HELIX")/dlss-loader/helixsr-fg"; do
    if [ -f "$d/nvexec64.dll" ] && [ -f "$d/_nvngx.dll" ] && [ -f "$d/nvngx_dlssg.dll" ]; then
        (cd "$d" && find . -type f -not -name '*.log' -not -name '*.log.*' | sed 's|^\./||') >"$TMP_FG.list"
        while IFS= read -r f; do
            mkdir -p "$TMP_FG/$(dirname "$f")" && cp -f "$d/$f" "$TMP_FG/$f"
        done <"$TMP_FG.list"
        rm -f "$TMP_FG.list"
        break
    fi
done

# Name for OptiScaler.dll: the one used before, else dxgi.dll unless the game has its own dxgi.dll
if [ -z "$NAME" ] && installed_in "$TARGET"; then
    NAME=$(sed -n 's/^name=//p' "$TARGET/$MANIFEST" | head -n 1)
fi
if [ -z "$NAME" ]; then
    for n in $NAMES; do
        [ -e "$TARGET/$n" ] || { NAME=$n; break; }
    done
    [ -n "$NAME" ] || NAME=dxgi.dll
fi
case " $NAMES OptiScaler.asi " in *" $NAME "*) ;; *) die "--name must be one of: $NAMES" ;; esac

SETTINGS=""
if installed_in "$TARGET"; then
    # an update keeps the game's OptiScaler.ini (the settings)
    if [ -f "$TARGET/OptiScaler.ini" ]; then
        SETTINGS=$(mktemp)
        cp -f "$TARGET/OptiScaler.ini" "$SETTINGS"
    fi
    remove_from "$TARGET" quiet
    ok "Old HelixSR Scaler taken out (updating, settings kept)"
fi

# The download's files go to a staging folder first: the download may have been extracted into the game itself
STAGE=$(mktemp -d)
trap 'rm -f "$TMP_HELIX"; rm -rf "$STAGE" "$TMP_FG"' EXIT
if [ -f "$HERE/$PACKAGE_LIST" ]; then
    tr -d '\r' <"$HERE/$PACKAGE_LIST" >"$STAGE/.list"
elif [ "$HERE" = "$TARGET" ]; then
    # without the list there's no telling the download's files from the game's
    die "$PACKAGE_LIST is missing. Extract the download to its own folder (not the game's) and run setup_linux.sh there."
else
    (cd "$HERE" && find . -type f -not -path './HelixSR/*' -not -path "./$BACKUP/*" | sed 's|^\./||' | sort) >"$STAGE/.list"
fi
while IFS= read -r rel; do
    [ -n "$rel" ] && [ -f "$HERE/$rel" ] || continue
    mkdir -p "$STAGE/$(dirname "$rel")" && cp -f "$HERE/$rel" "$STAGE/$rel" || die "Couldn't read $rel from the download"
done <"$STAGE/.list"
grep -qx OptiScaler.dll "$STAGE/.list" && [ -f "$STAGE/OptiScaler.dll" ] || die "OptiScaler.dll is missing from the download"
if [ "$HERE" = "$TARGET" ]; then
    # extracted into the game: those copies are HelixSR Scaler's, not the game's, so they aren't backed up
    while IFS= read -r rel; do
        case " $NOT_COPIED " in *" $rel "*) continue ;; esac
        [ -n "$rel" ] && rm -f "${TARGET:?}/$rel"
    done <"$STAGE/.list"
    rm -f "$TARGET/!! EXTRACT ALL FILES TO GAME FOLDER !!"
fi

# Copy, keeping what the game had before in the backup folder
: >"$TARGET/$MANIFEST.tmp"
echo "name=$NAME" >>"$TARGET/$MANIFEST.tmp"
backed_up=0
copy_one() { # copy_one SOURCE RELATIVE_DESTINATION
    local src="$1" rel="$2"
    if [ -e "$TARGET/$rel" ]; then
        mkdir -p "$TARGET/$BACKUP/$(dirname "$rel")"
        mv -f "$TARGET/$rel" "$TARGET/$BACKUP/$rel"
        backed_up=$((backed_up + 1))
    fi
    mkdir -p "$TARGET/$(dirname "$rel")"
    cp -f "$src" "$TARGET/$rel" || die "Couldn't copy $rel into the game"
    echo "$rel" >>"$TARGET/$MANIFEST.tmp"
}

while IFS= read -r rel; do
    case " $NOT_COPIED " in *" $rel "*) continue ;; esac
    case "$rel" in '' | "!! "* | HelixSR/* | "$FSRFG"/* | "$BACKUP"/* | "$MANIFEST"* | "$REMOVER") continue ;; esac
    dest="$rel"
    [ "$rel" = OptiScaler.dll ] && dest="$NAME"
    copy_one "$STAGE/$rel" "$dest"
done <"$STAGE/.list"
copy_one "$TMP_HELIX" "$FSRFG/amd_fidelityfx_dx12.dll"
FG=0
while IFS= read -r f; do
    copy_one "$TMP_FG/$f" "$FSRFG/helixsr-fg/$f"
    FG=1
done < <(cd "$TMP_FG" && find . -type f | sed 's|^\./||' | sort)
if [ -n "$SETTINGS" ]; then
    cp -f "$SETTINGS" "$TARGET/OptiScaler.ini"
    rm -f "$SETTINGS"
fi
# Frame generation is off unless switched on ([HelixSR] FrameGeneration=on); asked when OptiScaler.ini leaves it on auto
FG_ON=0
if [ $FG = 1 ] && [ -f "$TARGET/OptiScaler.ini" ]; then
    fg_now=$(tr -d '\r' <"$TARGET/OptiScaler.ini" | sed -n 's/^FrameGeneration[[:space:]]*=[[:space:]]*//p' | head -n 1)
    case "$fg_now" in
        on | true | 1) FG_ON=1 ;;
        auto)
            if ask "Turn on HelixSR frame generation in this game (DirectX 12 games)?"; then
                sed -i 's/^FrameGeneration[[:space:]]*=.*/FrameGeneration=on/' "$TARGET/OptiScaler.ini"
                FG_ON=1
            fi
            ;;
        *) ;;
    esac
fi
cp -f "$SELF" "$TARGET/$REMOVER" && chmod +x "$TARGET/$REMOVER"
mv -f "$TARGET/$MANIFEST.tmp" "$TARGET/$MANIFEST"

ok "HelixSR Scaler installed as ${B}$NAME${N}"
[ $backed_up -gt 0 ] && ok "$backed_up of the game's own files kept in ${D}$BACKUP${N}"
if [ $FG_ON = 1 ]; then
    ok "Frame generation included and on"
elif [ $FG = 1 ]; then
    ok "Frame generation included, off (Insert menu > Frame Generation > HelixSR, or FrameGeneration=on in OptiScaler.ini)"
else
    say "  ${D}Frame generation isn't set up (no helixsr-fg folder next to HelixSR's DLL): upscaling only${N}"
fi

# Launch options: Proton takes the game's own dxgi.dll by itself; for the other names it has to be told
DLL="${NAME%.*}"
if [ "$NAME" = dxgi.dll ]; then
    say ""
    ok "No launch options needed"
else
    OPTS="WINEDLLOVERRIDES=\"$DLL=n,b\" %command%"
    say ""
    say "  ${B}One last step: Steam launch options${N}"
    say ""
    say "  In Steam: right-click ${B}$GAME_TITLE${N} > Properties > General > Launch Options, and paste:"
    say ""
    say "      ${T}${B}$OPTS${N}"
    say ""
    if copy_to_clipboard "$OPTS"; then
        ok "Copied to the clipboard"
        say ""
    fi
    say "  ${D}Heroic or Lutris: add the environment variable WINEDLLOVERRIDES with the value $DLL=n,b${N}"
fi
say ""
say "  Then start the game and turn on ${V}DLSS${N} (and DLSS Frame Generation if it has it) in its graphics settings."
say "  Press ${B}Insert${N} for the menu: the upscaler list shows ${T}HelixSR${N}."
say "  ${D}Steam Deck: it has no Insert key. In the game's controller settings, set a back button to Insert.${N}"
say ""
say "  ${D}To remove it: run $REMOVER in the game's folder.${N}"
say ""
pause
# Extracted into the game: the download's own files go now (HelixSR-Scaler-remove.sh does the removing)
if [ "$HERE" = "$TARGET" ]; then
    for f in $NOT_COPIED; do rm -f "${TARGET:?}/$f"; done
fi
exit 0
