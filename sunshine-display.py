#!/usr/bin/python3
"""Save and restore the monitor mode around the Sunshine Yuri session."""
import json
import os
from pathlib import Path
import subprocess
import sys

state = Path(os.environ.get('XDG_STATE_HOME', str(Path.home() / '.local/state'))) / 'yuri-stream-display.json'

def change(name, mode):
    subprocess.run(['kscreen-doctor', f'output.{name}.mode.{mode}'], check=True)

if sys.argv[1:] == ['start']:
    if subprocess.run(['pgrep', '-x', 'gamemd.exe'], stdout=subprocess.DEVNULL).returncode == 0:
        raise SystemExit('Close Yuri before starting the streaming profile.')
    outputs = json.loads(subprocess.check_output(['kscreen-doctor', '-j']))['outputs']
    output = next(o for o in outputs if o['name'] == 'DP-2' and o['enabled'])
    modes = [m for m in output['modes'] if m['size'] == {'width': 2560, 'height': 1440}]
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
