#!/bin/bash
# Build a Release and package it as a portable zip in dist/.
#
# Usage: scripts/package-release.sh
#   The version comes from resources/resource.h (APP_VERSION_STR), so bump it
#   there first. Produces dist/SteamlessController-v<version>-win-x64.zip and a
#   .sha256 beside it, ready to attach to a GitHub release.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(grep -oP '#define APP_VERSION_STR\s+"\K[^"]+' "$REPO/resources/resource.h")
NAME="SteamlessController-v$VERSION-win-x64"

"$REPO/scripts/winbuild.sh" Release SteamlessController SteamlessDeviceCycle ViGEmBusProbe \
    | grep -E -i "error|warning|\.exe" || true

PROFILE_W=$(cmd.exe /c "echo %USERPROFILE%" 2>/dev/null | tr -d '\r')
OUT=$(wslpath -u "${WINBUILD_ROOT:-$PROFILE_W\\build}\\SteamlessController-build\\Release")
for f in SteamlessController.exe SteamlessDeviceCycle.exe ViGEmBusProbe.exe steam_input_gate.dll; do
    [ -f "$OUT/$f" ] || { echo "missing $OUT/$f - did the build fail?" >&2; exit 1; }
done

# The version the exe reports must match the zip's name.
mkdir -p "$REPO/dist"
STAGE=$(mktemp -d)
mkdir "$STAGE/SteamlessController"
cp "$OUT"/{SteamlessController.exe,SteamlessDeviceCycle.exe,ViGEmBusProbe.exe} \
   "$OUT"/steam_input_gate.{dll,LICENSE.txt,THIRD_PARTY_LICENSES.md} "$STAGE/SteamlessController/"
cp "$REPO"/{README.md,LICENSE,THIRD_PARTY_NOTICES.md} "$STAGE/SteamlessController/"

ZIP="$REPO/dist/$NAME.zip"
rm -f "$ZIP"
(cd "$STAGE" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for root, _, files in os.walk('SteamlessController'):
        for f in sorted(files):
            z.write(os.path.join(root, f))
" "$ZIP")
rm -rf "$STAGE"
(cd "$REPO/dist" && sha256sum "$NAME.zip" > "$NAME.zip.sha256")
echo "$ZIP"
cat "$REPO/dist/$NAME.zip.sha256"
