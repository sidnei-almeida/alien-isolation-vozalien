#!/usr/bin/env bash
# VozAlien installer for Linux (Steam + Proton).
#
#   curl -fsSL https://raw.githubusercontent.com/sidnei-almeida/alien-isolation-vozalien/main/install.sh | bash
#
# Options (when running the file directly):
#   --game-dir DIR   use this Alien: Isolation folder (the one with AI.exe)
#   --local          install dist/VozAlien.asi built from this clone instead of downloading
#   --uninstall      remove VozAlien.asi (keeps your VozAlien.ini and the log)
#   --dry-run        show what would be done, change nothing
set -euo pipefail

REPO="sidnei-almeida/alien-isolation-vozalien"
STEAM_SHA="dde66521798aae8c39ed4e7a3400f561fcecfff83619d1426b9e0cedf5819a90"
UAL_URL="https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v10.0.1/Ultimate-ASI-Loader-NoPDB.zip"
UAL_ZIP_SHA="acfec08aabf361b412db58726bde20df658b5430c043c241b6149c976cb63ab3"
UAL_DLL_SHA="8461bfd20d260087e76f9dbbaefb0bc237c44a83505b5a88cc9cb61404023e85"
LOADER_NAMES=(d3d11.dll winmm.dll dxgi.dll version.dll dinput8.dll dsound.dll winhttp.dll)

GAME_DIR="${GAME_DIR:-}"
LOCAL=0 UNINSTALL=0 DRY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --local) LOCAL=1; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

say()  { printf '\033[1;32m==>\033[0m %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf '\033[1;33m !!\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m xx\033[0m %s\n' "$*" >&2; exit 1; }
run()  { if [ "$DRY" = 1 ]; then info "(dry-run) $*"; else "$@"; fi; }
sha()  { sha256sum "$1" | cut -d' ' -f1; }
is_ual() { grep -q -a -E 'UltimateASILoader|Ultimate-ASI-Loader' "$1" 2>/dev/null; }

find_game() {
  local root vdf lib d
  for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.steam/root" \
              "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam" \
              "$HOME/snap/steam/common/.local/share/Steam"; do
    [ -d "$root" ] || continue
    vdf="$root/steamapps/libraryfolders.vdf"
    local libs=("$root")
    if [ -f "$vdf" ]; then
      while IFS= read -r lib; do libs+=("$lib"); done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf")
    fi
    for lib in "${libs[@]}"; do
      d="$lib/steamapps/common/Alien Isolation"
      if [ -f "$d/AI.exe" ]; then printf '%s\n' "$d"; return 0; fi
    done
  done
  return 1
}

say "VozAlien installer"
if [ -z "$GAME_DIR" ]; then
  GAME_DIR="$(find_game || true)"
  [ -n "$GAME_DIR" ] || die "Alien: Isolation not found in your Steam libraries. Use --game-dir \"/path/to/Alien Isolation\"."
fi
[ -f "$GAME_DIR/AI.exe" ] || die "AI.exe not found in: $GAME_DIR"
info "game: $GAME_DIR"

if [ "$UNINSTALL" = 1 ]; then
  run rm -f "$GAME_DIR/VozAlien.asi"
  say "VozAlien removed (VozAlien.ini and VozAlien.log were kept; the ASI loader was left in place)."
  exit 0
fi

if [ "$(sha "$GAME_DIR/AI.exe")" != "$STEAM_SHA" ]; then
  warn "AI.exe is not the Steam build VozAlien was made for. The mod checks the game's code at"
  warn "startup and stays disabled if it does not match (it will say so in VozAlien.log)."
fi

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fetch() { curl -fsSL --retry 3 -o "$2" "$1" || die "download failed: $1"; }

# 1) mod files
if [ "$LOCAL" = 1 ]; then
  SRC="$(cd "$(dirname "$0")" && pwd)"
  [ -f "$SRC/dist/VozAlien.asi" ] || die "dist/VozAlien.asi not found. Run ./build.sh first."
  cp "$SRC/dist/VozAlien.asi" "$TMP/"; cp "$SRC/VozAlien.ini" "$TMP/"
  info "using local build"
else
  say "Downloading the latest release"
  fetch "https://github.com/$REPO/releases/latest/download/VozAlien.asi" "$TMP/VozAlien.asi"
  fetch "https://github.com/$REPO/releases/latest/download/VozAlien.ini" "$TMP/VozAlien.ini"
fi

# 2) the older AlienIsolationVoiceRestore mod would emit its own events on top of ours
if [ -f "$GAME_DIR/AlienIsolationVoiceRestore.asi" ]; then
  warn "AlienIsolationVoiceRestore.asi found: renaming it to .asi.disabled so both mods don't run together."
  run mv -f "$GAME_DIR/AlienIsolationVoiceRestore.asi" "$GAME_DIR/AlienIsolationVoiceRestore.asi.disabled"
fi

# 3) ASI loader
LOADER=""
for n in "${LOADER_NAMES[@]}"; do
  if [ -f "$GAME_DIR/$n" ] && is_ual "$GAME_DIR/$n"; then LOADER="$n"; break; fi
done
NEW_LOADER=0
if [ -n "$LOADER" ]; then
  info "ASI loader already installed: $LOADER"
else
  if [ ! -e "$GAME_DIR/d3d11.dll" ]; then LOADER="d3d11.dll"
  elif [ ! -e "$GAME_DIR/winmm.dll" ]; then LOADER="winmm.dll"
  else die "No ASI loader found and both d3d11.dll and winmm.dll are taken by other mods. Install Ultimate ASI Loader manually."; fi
  say "Installing Ultimate ASI Loader v10.0.1 (ThirteenAG, MIT) as $LOADER"
  fetch "$UAL_URL" "$TMP/ual.zip"
  [ "$(sha "$TMP/ual.zip")" = "$UAL_ZIP_SHA" ] || die "Ultimate ASI Loader archive checksum mismatch."
  if command -v unzip >/dev/null; then unzip -p "$TMP/ual.zip" dinput8.dll > "$TMP/ual.dll"
  elif command -v bsdtar >/dev/null; then bsdtar -xOf "$TMP/ual.zip" dinput8.dll > "$TMP/ual.dll"
  elif command -v python3 >/dev/null; then python3 -I -c 'import sys,zipfile; sys.stdout.buffer.write(zipfile.ZipFile(sys.argv[1]).read("dinput8.dll"))' "$TMP/ual.zip" > "$TMP/ual.dll"
  else die "Need unzip, bsdtar or python3 to extract the loader."; fi
  [ "$(sha "$TMP/ual.dll")" = "$UAL_DLL_SHA" ] || die "Ultimate ASI Loader DLL checksum mismatch."
  run cp "$TMP/ual.dll" "$GAME_DIR/$LOADER"
  NEW_LOADER=1
fi

# 4) VozAlien (atomic rename: safe even if the game is running)
say "Installing VozAlien"
run cp "$TMP/VozAlien.asi" "$GAME_DIR/.VozAlien.asi.new"
run mv -f "$GAME_DIR/.VozAlien.asi.new" "$GAME_DIR/VozAlien.asi"
if [ -f "$GAME_DIR/VozAlien.ini" ]; then
  info "kept your existing VozAlien.ini"
else
  run cp "$TMP/VozAlien.ini" "$GAME_DIR/VozAlien.ini"
fi

say "Done."
OVR="${LOADER%.dll}=n,b"
echo
echo "  Proton needs to load the ASI loader ($LOADER) from the game folder."
echo "  In Steam: Alien: Isolation > Properties > Launch Options, make sure it contains:"
echo
echo "      WINEDLLOVERRIDES=\"$OVR\" %command%"
echo
echo "  If you already have a WINEDLLOVERRIDES (e.g. for ReShade), add $OVR to it with a ';'."
[ "$NEW_LOADER" = 1 ] || echo "  (You already had a loader, so this is probably set up already.)"
echo
echo "  First launch: stay quiet for ~3 seconds when the game window opens (room-noise calibration)."
echo "  Log: $GAME_DIR/VozAlien.log"
