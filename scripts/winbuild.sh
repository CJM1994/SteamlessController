#!/bin/bash
# Build the Windows targets with MSVC from WSL.
#
# MSVC and the Windows SDK only run on Windows, and CMake cannot build from a
# \\wsl$ path, so the tracked working tree (plus untracked, non-ignored files)
# is mirrored to a folder on the Windows side and built there with the Visual
# Studio Build Tools found by vswhere.
#
# Usage: scripts/winbuild.sh [Debug|Release] [target...]
#   No targets builds everything. Outputs land in
#   %USERPROFILE%\build\SteamlessController-build\<Config>\.
#
# Override the Windows-side root with WINBUILD_ROOT (a Windows path).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
CONFIG=${1:-Debug}; shift || true
TARGETS=""; for t in "$@"; do TARGETS="$TARGETS --target $t"; done

winpath_to_wsl() { wslpath -u "$1"; }

PROFILE_W=$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r')
ROOT_W=${WINBUILD_ROOT:-"$PROFILE_W\\build"}
SRC_W="$ROOT_W\\SteamlessController-src"
BLD_W="$ROOT_W\\SteamlessController-build"
ROOT=$(winpath_to_wsl "$ROOT_W")
SRC=$(winpath_to_wsl "$SRC_W")

VSWHERE="/mnt/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[ -x "$VSWHERE" ] || { echo "vswhere not found - install the Visual Studio Build Tools" >&2; exit 1; }
VS_W=$("$VSWHERE" -latest -products '*' \
         -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
         -property installationPath | tr -d '\r')
[ -n "$VS_W" ] || { echo "No Visual Studio with the C++ tools found" >&2; exit 1; }

# Mirror: start clean so deleted files do not linger, skip the untracked
# reference material, and skip tracked files deleted from the working tree.
rm -rf "$SRC"; mkdir -p "$SRC"
cd "$REPO"
git ls-files -co --exclude-standard | grep -v '^reference/' \
  | while read -r f; do [ -e "$f" ] && echo "$f"; done \
  | tar -cf - -T - | tar -xf - -C "$SRC"

# Through a batch file: cmd.exe's quoting of a path with spaces and parentheses
# passed inline from bash is not worth fighting.
cat > "$ROOT/winbuild.bat" <<BAT
@echo off
call "$VS_W\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1
cmake -S "$SRC_W" -B "$BLD_W" -G "Visual Studio 17 2022" -A x64 >nul || exit /b 1
cmake --build "$BLD_W" --config $CONFIG $TARGETS -- /m /v:minimal /nologo
BAT
cd "$ROOT" && cmd.exe /c winbuild.bat 2>&1 | tr -d '\r'
