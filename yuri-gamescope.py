#!/usr/bin/python3
"""Keep the tested game resolution; fit Gamescope to the active monitor.

DP-2 is the monitor the game is set up for. With DP-2 off, another enabled monitor is used; with
none at all (the screen asleep during overnight benchmark runs), Gamescope runs headless."""
import json
import os
import subprocess
import sys

try:
    outputs = json.loads(subprocess.check_output(['kscreen-doctor', '-j']))['outputs']
except (OSError, subprocess.CalledProcessError, ValueError, KeyError):
    outputs = []
enabled = [o for o in outputs if o.get('enabled')]
output = next((o for o in enabled if o['name'] == 'DP-2'), enabled[0] if enabled else None)
args = ['gamescope', '-w', '2560', '-h', '1440']
if output:
    mode = next(m for m in output['modes'] if m['id'] == output['currentModeId'])
    args += ['-W', str(mode['size']['width']), '-H', str(mode['size']['height']), '-f']
else:
    args += ['--backend', 'headless']
os.execvp('gamescope', [*args, '--', *sys.argv[1:]])
