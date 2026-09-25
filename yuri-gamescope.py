#!/usr/bin/python3
"""Keep the tested game resolution; fit Gamescope to the active monitor."""
import json
import os
import subprocess
import sys

outputs = json.loads(subprocess.check_output(['kscreen-doctor', '-j']))['outputs']
output = next(o for o in outputs if o['name'] == 'DP-2' and o['enabled'])
mode = next(m for m in output['modes'] if m['id'] == output['currentModeId'])
size = mode['size']
os.execvp('gamescope', ['gamescope', '-w', '2560', '-h', '1440',
                      '-W', str(size['width']), '-H', str(size['height']),
                      '-f', '--', *sys.argv[1:]])
