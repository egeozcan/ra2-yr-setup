#!/usr/bin/python3
"""Save and restore the monitor mode around the Sunshine Yuri session.

The monitor is the one named by RA2YR_OUTPUT (a kscreen-doctor output name such as DP-2), else the
primary one. It is set to the game's resolution (RA2MD.INI) at the rate closest to 60 Hz. Needs KDE's
kscreen-doctor."""
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mod'))
import ra2paths

state = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'yuri-stream-display.json'

def kscreen(*args):
    try:
        return subprocess.check_output(['kscreen-doctor', *args])
    except FileNotFoundError:
        raise SystemExit('sunshine-display.py needs kscreen-doctor (KDE Plasma).')

def change(name, mode):
    kscreen(f'output.{name}.mode.{mode}')

if sys.argv[1:] == ['start']:
    if subprocess.run(['pgrep', '-x', 'gamemd.exe|gamemd-spawn.ex'], stdout=subprocess.DEVNULL).returncode == 0:
        raise SystemExit('Close Yuri before starting the streaming profile.')
    outputs = json.loads(kscreen('-j'))['outputs']
    enabled = sorted((o for o in outputs if o['enabled']), key=lambda o: o.get('priority') or 99)
    if not enabled:
        raise SystemExit('No monitor is on.')
    output = next((o for o in enabled if o['name'] == os.environ.get('RA2YR_OUTPUT')), enabled[0])
    width, height = ra2paths.screen_size()
    modes = [m for m in output['modes'] if m['size'] == {'width': width, 'height': height}]
    if not modes:
        raise SystemExit(f"{output['name']} has no {width}x{height} mode.")
    mode = min(modes, key=lambda m: abs(m['refreshRate'] - 60))
    state.parent.mkdir(parents=True, exist_ok=True)
    if not state.exists():
        state.write_text(json.dumps({'output': output['name'], 'mode': output['currentModeId']}))
    try:
        change(output['name'], mode['id'])
    except Exception:
        saved = json.loads(state.read_text())
        change(saved['output'], saved['mode'])
        state.unlink()
        raise
elif sys.argv[1:] == ['stop']:
    if state.exists():
        saved = json.loads(state.read_text())
        change(saved['output'], saved['mode'])
        state.unlink()
else:
    raise SystemExit('Usage: sunshine-display.py start|stop')
