#!/usr/bin/env bash
# Revert the GAME DIRECTORY to EA stock. Does NOT touch the prefix override or Steam launch options
# (see HANDOFF.md "establish this FIRST" if you want a full original-stack revert).
set -euo pipefail
G="/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
D="$(cd "$(dirname "$0")" && pwd)"
pgrep -x gamemd.exe | while read -r p; do kill -TERM "$p"; done || true
pgrep -x game.exe   | while read -r p; do kill -TERM "$p"; done || true
sleep 3
cp -v "$D/backups/stock/ddraw.dll"       "$G/ddraw.dll"
cp -v "$D/backups/stock/DDrawCompat.ini" "$G/DDrawCompat.ini"
cp -v "$D/backups/stock/RA2.INI"         "$G/RA2.INI"
cp -v "$D/backups/stock/RA2MD.INI"       "$G/RA2MD.INI"
rm -fv "$G/ddraw.ini"
echo
echo "Game dir restored to EA stock (DDrawCompat, 640x480 / 1024x768)."
echo "NOTE: the prefix still has \"ddraw\"=\"native,builtin\" — with stock DDrawCompat that CRASHES"
echo "      the game under Proton. Remove the override too:"
echo "      cp '$D/backups/prefix/user.reg.before-ddraw-override' \\"
echo "         /mnt/data/SteamLibrary/steamapps/compatdata/2229850/pfx/user.reg"
echo "      and clear the Steam launch options."
