#!/usr/bin/python3
"""Run the game in Gamescope at its own resolution (RA2MD.INI), fitted to the active monitor.

The monitor is the one named by RA2YR_OUTPUT (a kscreen-doctor output name such as DP-2), else the
primary one. With that one off, another enabled monitor is used; with none at all (the screen asleep
during overnight benchmark runs), Gamescope runs headless. Without kscreen-doctor (not KDE),
Gamescope picks the output size itself."""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mod'))
import ra2paths

try:
    outputs = json.loads(subprocess.check_output(['kscreen-doctor', '-j']))['outputs']
except FileNotFoundError:
    outputs = None
except (OSError, subprocess.CalledProcessError, ValueError, KeyError):
    outputs = []
enabled = sorted((o for o in outputs or [] if o.get('enabled')), key=lambda o: o.get('priority') or 99)
output = next((o for o in enabled if o['name'] == os.environ.get('RA2YR_OUTPUT')), enabled[0] if enabled else None)
# Steam (and spawn.py) pass the game directory being launched
width, height = ra2paths.screen_size(os.environ.get('STEAM_COMPAT_INSTALL_PATH'))
args = ['gamescope', '-w', str(width), '-h', str(height)]
if outputs is None:
    args += ['-f']
elif output:
    mode = next(m for m in output['modes'] if m['id'] == output['currentModeId'])
    args += ['-W', str(mode['size']['width']), '-H', str(mode['size']['height']), '-f']
else:
    args += ['--backend', 'headless']
os.execvp('gamescope', [*args, '--', *sys.argv[1:]])
