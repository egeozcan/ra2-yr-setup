#!/usr/bin/env bash
# Re-apply the verified-working 2560x1440 setup (cnc-ddraw 6.3 + prefix override + gamescope).
set -euo pipefail
G="/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
D="$(cd "$(dirname "$0")" && pwd)"
PFX=/mnt/data/SteamLibrary/steamapps/compatdata/2229850/pfx
pgrep -x gamemd.exe | while read -r p; do kill -TERM "$p"; done || true
pgrep -x game.exe   | while read -r p; do kill -TERM "$p"; done || true
sleep 3
cp -v "$D/backups/working/ddraw.dll" "$G/ddraw.dll"
cp -v "$D/backups/working/ddraw.ini" "$G/ddraw.ini"
cp -v "$D/backups/working/RA2.INI"   "$G/RA2.INI"
cp -v "$D/backups/working/RA2MD.INI" "$G/RA2MD.INI"
# ensure Wine actually loads the game-dir wrapper (without this, everything above is a no-op)
if grep -q '"ddraw"="native,builtin"' "$PFX/user.reg"; then
  echo "prefix override: already present"
else
  cp -v "$PFX/user.reg" "$PFX/user.reg.bak.$(date +%s)"
  python3 - "$PFX/user.reg" <<'PY'
import sys
p=sys.argv[1]; s=open(p,encoding='utf-8',errors='replace').read()
i=s.find('[Software\\\\Wine\\\\DllOverrides]')
j=s.find('\n', s.find('#time=', i))+1
open(p,'w',encoding='utf-8',newline='').write(s[:j]+'"ddraw"="native,builtin"\n'+s[j:])
print("prefix override: inserted")
PY
fi
echo
echo "Done. Steam launch options must be:  gamescope -w 2560 -h 1440 -W 5120 -H 1440 -f -- %command%"
echo "Verify the wrapper loads once running:"
echo "  grep -i ddraw /proc/\$(pgrep -x gamemd.exe)/maps    # must show the GAME DIR ddraw.dll"
