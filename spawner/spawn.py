#!/usr/bin/env python3
"""Launch Yuri's Revenge straight into a skirmish, skipping the menus.

usage: spawn.py install        build yspawn.dll, make gamemd-spawn.exe, copy both into the game dir
       spawn.py uninstall      remove everything install and run put into the game dir
       spawn.py run [MAP]      start a skirmish with the settings in yspawn.ini (MAP overrides Map=)

gamemd-spawn.exe is a copy of the game's gamemd.exe that also imports yspawn.dll (add_import.py);
the stock gamemd.exe, and so the normal Steam launch, is left untouched. yspawn.dll replaces the
main menu with a routine that sets up the skirmish from yspawn.ini (see yspawn.c). When the match
ends the game exits instead of returning to the menu.
"""
import configparser, os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "mod"))
import mixextract
from add_import import main as add_import

GAME = "/mnt/data/SteamLibrary/steamapps/common/Command & Conquer Red Alert II"
PREFIX = "/mnt/data/SteamLibrary/steamapps/compatdata/2229850"
STEAM = os.path.expanduser("~/.local/share/Steam")
GAMESCOPE = os.path.join(HERE, "..", "yuri-gamescope.py")
FILES = ["gamemd-spawn.exe", "yspawn.dll", "yspawn.ini", "yspawn.map", "yspawn.log"]


def running():
    # Wine names the process after the exe, cut to 15 characters
    return subprocess.run(["pgrep", "-x", "gamemd.exe|gamemd-spawn.ex"], capture_output=True).returncode == 0


def sync_ddraw_ini():
    """cnc-ddraw picks its per-game section by exe name. Its stock [gamemd-spawn] section (meant for CnCNet)
    lacks windowed=false, so give it the same settings as the tested [gamemd] section."""
    path = os.path.join(GAME, "ddraw.ini")
    lines = open(path, newline="", encoding="latin-1").read().split("\r\n")

    def body(name):
        s = lines.index(f"[{name}]")
        e = next((i for i in range(s + 1, len(lines)) if lines[i].startswith("[")), len(lines))
        while e > s + 1 and (not lines[e - 1].strip() or lines[e - 1].startswith(";")):
            e -= 1   # keep the blank line and the comment that belong to the next section
        return s, e

    s, e = body("gamemd")
    settings = lines[s + 1:e]
    s, e = body("gamemd-spawn")
    if lines[s + 1:e] != settings:
        lines[s + 1:e] = settings
        open(path, "w", newline="", encoding="latin-1").write("\r\n".join(lines))
        print("ddraw.ini: [gamemd-spawn] now matches [gamemd]")


def install():
    subprocess.run([os.path.join(HERE, "build.sh")], check=True)
    add_import(os.path.join(GAME, "gamemd.exe"), os.path.join(GAME, "gamemd-spawn.exe"))
    shutil.copy(os.path.join(HERE, "yspawn.dll"), GAME)
    sync_ddraw_ini()
    print("installed gamemd-spawn.exe, yspawn.dll")


def uninstall():
    for f in FILES:
        p = os.path.join(GAME, f)
        if os.path.exists(p):
            os.remove(p)
            print("removed", f)


def read_config():
    ini = configparser.ConfigParser(inline_comment_prefixes=(";",), interpolation=None)
    ini.optionxform = str
    ini.read(os.path.join(HERE, "yspawn.ini"))
    return ini


def run(map_override=None):
    if not os.path.exists(os.path.join(GAME, "gamemd-spawn.exe")):
        raise SystemExit("not installed; run: spawn.py install")
    ini = read_config()
    settings = ini["Settings"]
    archive = map_override or settings.pop("Map")
    settings.pop("Map", None)
    # a skirmish map file is <name>.map inside the .mmx/.yro archive of the same name
    data = open(os.path.join(GAME, archive), "rb").read()
    base = os.path.splitext(os.path.basename(archive))[0].lower()
    open(os.path.join(GAME, "yspawn.map"), "wb").write(mixextract.extract(data, base + ".map"))
    settings["Scenario"] = "yspawn.map"
    # write a comment-free copy with CRLF line endings for GetPrivateProfile*
    lines = []
    for sec in ini.sections():
        lines += [f"[{sec}]"] + [f"{k}={v}" for k, v in ini[sec].items()] + [""]
    open(os.path.join(GAME, "yspawn.ini"), "w", newline="\r\n").write("\n".join(lines))
    print(f"map {archive}; starting")

    env = dict(os.environ, STEAM_COMPAT_DATA_PATH=PREFIX, STEAM_COMPAT_CLIENT_INSTALL_PATH=STEAM,
               STEAM_COMPAT_INSTALL_PATH=GAME, STEAM_COMPAT_APP_ID="2229850",
               SteamAppId="2229850", SteamGameId="2229850")
    cmd = ["/usr/bin/python3", GAMESCOPE,
           os.path.join(STEAM, "steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"),
           "--verb=waitforexitandrun", "--",
           os.path.join(STEAM, "steamapps/common/Proton - Experimental/proton"),
           "waitforexitandrun", os.path.join(GAME, "gamemd-spawn.exe")]
    subprocess.Popen(cmd, cwd=GAME, env=env, start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd not in ("install", "uninstall", "run"):
        raise SystemExit(__doc__)
    if running():
        raise SystemExit("Yuri's Revenge is running; close it first.")
    if cmd == "install":
        install()
    elif cmd == "uninstall":
        uninstall()
    else:
        run(*sys.argv[2:3])
