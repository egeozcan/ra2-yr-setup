#!/usr/bin/env bash
# Re-apply the verified-working 2560x1440 setup (cnc-ddraw 6.3 + prefix override + gamescope).
# From the saved files in backups/working/ if there are any; otherwise install-renderer.py builds
# the same setup from the public cnc-ddraw release (pass a resolution, e.g. 1920x1080, to change it).
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
# Game dir and Proton prefix: found through Steam, or set RA2YR_GAME / RA2YR_PREFIX (mod/ra2paths.py)
G="$(python3 "$D/mod/ra2paths.py" game)"
PFX="$(python3 "$D/mod/ra2paths.py" prefix)/pfx"
# Wine names the Skirmish Setup launcher's process gamemd-spawn.ex (15 characters)
GAMES='gamemd.exe|gamemd-spawn.ex|game.exe'
pkill -TERM -x "$GAMES" || true
for _ in $(seq 20); do pgrep -x "$GAMES" >/dev/null || break; sleep 1; done
if pgrep -x "$GAMES" >/dev/null; then
  echo "the game is still running; close it first" >&2   # cp would overwrite its loaded ddraw.dll
  exit 1
fi
if [ -d "$D/backups/working" ] && [ $# -eq 0 ]; then
  cp -v "$D/backups/working/ddraw.dll" "$G/ddraw.dll"
  cp -v "$D/backups/working/ddraw.ini" "$G/ddraw.ini"
  cp -v "$D/backups/working/RA2.INI"   "$G/RA2.INI"
  cp -v "$D/backups/working/RA2MD.INI" "$G/RA2MD.INI"
else
  python3 "$D/install-renderer.py" "$@"
fi
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
echo "Done. Steam launch options (Gamescope at the game's resolution, fitted to the monitor):"
echo "  /usr/bin/python3 '$D/yuri-gamescope.py' %command%"
echo "Verify the wrapper loads once running:"
echo "  grep -i ddraw /proc/\$(pgrep -x gamemd.exe)/maps    # must show the GAME DIR ddraw.dll"
